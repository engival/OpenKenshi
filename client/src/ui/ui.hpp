#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sokol_app.h"
#include "ui/widget.hpp"

// The UI context (docs/UI.md): widget layers, scale, pointer and keyboard routing. Main thread only.
//
// Host contract (kclient main.cpp, kui_demo):
//   ui::init(dataDir, themes) after sg/sgl setup; false = UI assets unusable (the client runs without a UI). themes: folders whose gui/
//                             files replace the game's, last wins per file (an enabled Dark UI mod's folder); res::file resolves a path
//                             the same way for host-loaded gui art (cursors, loading screens)
//   if (ui::event(e)) return; first in the event callback: the UI sees every event and says whether it used it
//   ui::setViewport(w, h)     each frame before ui::frame: the framebuffer size in pixels (the UI calls no sapp size function)
//   ui::frame(dt)             once per frame before drawing: scale, layout, timers
//   ui::record(list)          once per frame after ui::frame: the UI's draws and texture uploads into a dl::List (drawlist.hpp; no sokol);
//                             the render side applies its uploads and replays it (dl::gpu::apply / replay) inside the swapchain pass after
//                             the world and HUD text, before the cursor
//   ui::shutdown()            before dl::gpu::shutdown (it records the destroys of the UI's textures)
//   ui::setClipboardFns(g, s) once, optional: the system clipboard for the edit boxes (none set: no clipboard; the UI calls no sapp clipboard function)
namespace ui
{
	bool init(const std::string& dataDir, const std::vector<std::string>& themes = {});
	void shutdown();
	bool ready();

	// Screens add their widgets here; it covers the screen (its rect is the screen in units).
	Widget& root();

	// true when the UI used the event; the world must then ignore it. Mouse moves are never consumed (the host keeps tracking the pointer);
	// ask wantsMouse() before hover-picking in the world.
	bool event(const sapp_event& e);
	void setViewport(int w, int h);	// framebuffer pixels; ui::frame lays out and ui::record draws at this size
	void frame(double dt);
	void record(dl::List& list);	// list.begin(viewport) and the frame's draws, plus every texture upload made since the last record
	// The edit boxes' copy / paste; an empty function: that half does nothing (paste inserts nothing).
	void setClipboardFns(std::function<std::string()> get, std::function<void(const std::string&)> set);

	float scale();		// pixels per unit: framebuffer height / 1080 x ui.scale %
	bool wantsMouse();	// the pointer is over a solid widget or the UI holds a press
	void mouse(float& x, float& y);	// last pointer position, pixels
	double now();		// UI clock, seconds (advanced by frame)
	uint32_t mods();	// SAPP_MODIFIER_* of the last input event

	// Interaction state, for widgets' looks.
	bool hovered(const Widget* w);
	// Fixed tip boxes (the original HUD's TooltipPanel): while claimed, the tips of the widgets under area float no tooltip; the screen shows
	// tipIn(area) in its own box instead: the hovered widget's tip when it is under area, at once (no delay), else empty.
	void claimTips(const Widget* area, bool on);
	std::string tipIn(const Widget* area);
	std::string claimedTip();	// tipIn over every claimed area: the HUD's box shows the other screens' fixed tips too (the inventory's stats rows)
	bool fixedTips();	// ui.hud_tips=box: screens claim their fixed-box tips; float: they float at the pointer
	Widget* hoveredWidget();	// the widget under the pointer (the tooltip's), null over the world

	// Control hints panel (docs/UI.md "Control hints", kenshi-k16.2.294): a small see-through list on the right edge, one line per input
	// of the thing under the pointer. The host calls setHints each frame from its resolver (title = the target); an empty block hides it.
	// While the pointer is over a widget (or under one) whose hints() lists lines, those show instead. Off unless ui.control_hints (F6).
	struct HintBlock
	{
		std::string title;
		std::vector<HintLine> lines;
	};
	void setHints(const HintBlock& b);
	bool hintsShown();	// the panel is up this frame (setting on and lines to show)
	HintBlock shownHints();	// what it shows (scripted tests), empty when hidden

	bool pressed(const Widget* w);
	bool focused(const Widget* w);
	void focus(Widget* w);
	Widget* focusedWidget();	// the keyboard's widget, null when the world has it

	// Popup layer: a menu at (x, y) pixels, kept on screen (clamped at the right and bottom). held (hold-RMB menus; the press that opened it is
	// still down): the next release of any button picks the row under it in any held menu, or closes them all elsewhere. add: keep the open
	// popups (a building's menu and its occupant's, both held). rightEdge: x is the menu's right edge, not kept on screen at the left (ctx18_static).
	Menu& openMenu(std::unique_ptr<Menu> m, float x, float y, bool held = false, bool add = false, bool rightEdge = false);
	void closePopups();
	bool popupsOpen();	// any menu showing

	// The hold-RMB order menu (ContextMenu) at (x, y) pixels, opened while the button is still held (the host keeps the 0.2 s hold): the
	// title (the target's name) and the rows in the host's order, every one doable. A release of any button over a row closes the menu, then
	// calls onPick with the row's index; elsewhere it only closes. value: a chance 0..100, shown "N%" beside the row; -1 none. second: the
	// occupant's menu beside an open one (a bed and its sleeper), its right edge at x.
	struct ContextRow
	{
		std::string label;
		int value = -1;
	};
	ContextMenu& openContextMenu(float x, float y, const std::string& title, const std::vector<ContextRow>& rows, std::function<void(int)> onPick, bool second = false);
	bool contextMenuShown();

	// UI sounds (kenshi-k16.3.17; kenshi-inspect findings/audio_sfx_triggers.md sections 2, 3, 5): the widgets and screens post the original's
	// Wwise events by name (General_Click, Hover, Tab, Slider, Stats_Open, ...), with at most one switch (group = value) the host sets on the
	// GUI object (110) before the post. The host hashes the names (FNV-1 32 of the lowercase name). No sink, or ui.sounds off: nothing is posted.
	struct SoundPost
	{
		std::string event, group, value;
	};
	void setSoundSink(std::function<void(const SoundPost&)> sink);
	void sound(const std::string& event, const std::string& group = {}, const std::string& value = {});
	// A picture carried under the pointer (drag and drop), painted above everything at the pointer (pixels); an empty function clears it.
	void setDragImage(std::function<void(float x, float y)> paint);
	// Sees every event before the UI while set (an item lifted by a click: the next click anywhere is its); returns true when it used it.
	void setInterceptor(std::function<bool(const sapp_event&)> f);
	// A message on the toast line (top centre).
	void toast(const std::string& text, const Color& c, double seconds = 4);
	void toast(const std::string& text);
	Toasts* toasts();	// for scripted tests
	// The HUD message box, bottom centre above the squad tabs ("You can't afford that.", "Out of trading range."): the original's roller, one at a
	// time in order, each held `seconds` then fading 0.33 s; a text already up or waiting is dropped; "" clears them. The default colour is the
	// box's own text colour. The build window moves it over its main panel while open.
	void message(const std::string& text, const Color& c, double seconds = 4);
	void message(const std::string& text);
	HudMessage* messageBox();	// for scripted tests, and the build window (moves it while open)
	// The zone banner: a big name across the upper middle ("The Great Desert" on entering a zone), fading out over the second half.
	void banner(const std::string& text, double seconds = 6);
	// Drops the toasts, the message box and the banner (the game's news: a root screen over the game, the title, clears them).
	void clearMessages();
	bool messagesShown();	// any of them still up (for scripted tests)

	namespace detail { void forget(const Widget* w); }
}
