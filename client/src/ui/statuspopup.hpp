#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The status popup (docs/UI.md "Status popup"; user 2026-11-20, kenshi-k16.2.445): a small panel at the top right listing what is wrong or busy
// ("Server loading", "Ping to server lagging", "Connection lost"), one line each. It shows only while the host's state holds a line and hides
// itself when the list empties (after the last line's fade): no show() for the host to manage. Lines fade in and out over kFade; a line keyed
// as before keeps its row (no flicker, its busy cue runs on). No input: clicks pass through, except on a line when StatusPopupActions::clicked
// is set (later: "reconnect").
//
// Host use, every frame (or whenever the status changes):
//	ui::StatusPopup sp(actions);	// once, after ui::init
//	sp.update(state);	// cheap when nothing changed
namespace ui
{
	struct StatusLine
	{
		enum class Level { kInfo, kWarn, kError };
		std::string key;	// identifies the line across updates (a line that stays keeps its row and its cue)
		std::string text;
		Level level = Level::kInfo;	// the text's colour: Main / Accent / BadBright
		bool busy = false;	// a working cue (three dots, one lit in turn, as the main menu's server panel) before the text
		float seconds = -1;	// elapsed, shown at the right ("12 s", "2 m 05 s") when >= 0; hidden when < 0
		bool operator==(const StatusLine&) const = default;
	};

	struct StatusPopupState
	{
		std::vector<StatusLine> lines;	// top down; at most kMaxLines show (the rest are dropped); empty: the popup goes
		Rect avoid;	// pixels: a window to sit beside (the minimap's screen rect); w or h <= 0: none. Set: the popup sits just left of it,
				// top-aligned; none: the top right corner, StatusPopup::kMargin in and kTop down
		bool operator==(const StatusPopupState& o) const
		{
			return lines == o.lines && avoid.x == o.avoid.x && avoid.y == o.avoid.y && avoid.w == o.avoid.w && avoid.h == o.avoid.h;
		}
	};

	struct StatusPopupActions
	{
		std::function<void(const std::string& key)> clicked;	// a left click on a line (unset: the popup takes no input at all)
	};

	class StatusPopup
	{
	public:
		static constexpr size_t kMaxLines = 5;
		static constexpr double kFade = 0.3;	// seconds, a line's fade in and out
		static constexpr float kMargin = 16, kTop = 97, kGap = 8;	// units: right margin and top without an avoid rect (the minimap's); the gap beside it
		static constexpr float kMinW = 220, kMaxW = 520;	// units: the panel's width range (fit to the widest line)

		explicit StatusPopup(StatusPopupActions actions = {});
		~StatusPopup();
		StatusPopup(const StatusPopup&) = delete;
		StatusPopup& operator=(const StatusPopup&) = delete;
		void update(const StatusPopupState& s);
		bool shown() const;	// the window is up (a line showing or fading out)
		// For scripted tests.
		Widget* window() const;
		Widget* row(const std::string& key) const;	// the line's row (fading out included); nullptr when none
		float alpha(const std::string& key) const;	// the line's fade, 0..1; 0 when none

	private:
		class View;
		class Row;
		View* view_ = nullptr;
		StatusPopupActions act_;
		StatusPopupState last_;
	};
}
