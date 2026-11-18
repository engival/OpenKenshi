#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "ui/widget.hpp"

// The options window (docs/UI.md): every declared cfg option, one tab per key prefix (audio, ui, mod, ...), built from cfg::list(), so a new option
// shows up without touching this file. Changes are user sets (cfg::setUser; cfg::poll saves them a second later). An option pinned on the command
// line shows the value in effect but is disabled. Values follow edits of the file made while it is open.
namespace ui
{
	class Options
	{
	public:
		Options();
		~Options();
		void update();	// every frame, before ui::frame
		bool shown() const;
		void show(bool on);
		// For scripted tests; null when out of range / unknown.
		Widget* tab(size_t i) const { return i < tabs_.size() ? tabs_[i].button : nullptr; }
		Widget* control(const std::string& key) const;	// a key row's: its first KeyField
		Widget* keyField(const std::string& action, int slot) const;	// on the shown Keys tab
		Widget* closeButton() const { return close_; }

	private:
		struct Tab { std::string prefix; Button* button; };
		struct Row { cfg::Info info; Widget* box; Widget* control; Label* value; Widget* swatch = nullptr; Widget* keys[2] = {}; };
		CaptionWindow* window_ = nullptr;
		TabView* tabView_ = nullptr;
		ScrollView* rows_ = nullptr;
		Label* help_ = nullptr;
		Button* close_ = nullptr;
		std::vector<Tab> tabs_;
		std::vector<Row> shownRows_;
		size_t current_ = 0;
		uint64_t gen_ = 0;
		size_t optionCount_ = 0;
		void build();
		void select(size_t tab);
		void refresh();
	};
}
