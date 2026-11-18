#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The options window (docs/UI.md): every declared cfg option, one tab per cfg section (cfg::sectionNames(): Graphics, Post-processing, ...), a header
// per group (cfg::Info::group) with its options under it and an option's needs note under its row (dimmed while what it needs is off; kenshi-k16.2.244.2),
// built from cfg::list(), so a new option shows up without touching this file. A resizable window: the tabs a side list (SETTINGS, then CLIENT), each row a Flex (name, the control
// filling the middle, the value at the right edge). The body's layout is client/data/ui/options.layout (ui::Layout; reloads when saved). Changes are user sets (cfg::setUser; cfg::poll saves them a second later). An option pinned on the command
// line (--set) works as any other: its tooltip and help say so, and a change ends the pin and is saved (k16.2.303.1). Values follow edits of the file made while it is open. The Find bar over it (Ctrl+F) steps
// through the rows of every tab whose label, key or help has the text, best match first (label matches before key and help ones; Next / Enter,
// Previous / Shift+Enter, wrapping, jumping between tabs): the hit's tab shown,
// the row scrolled into view and highlighted; nothing is hidden. Escape clears it.
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
		CaptionWindow* window() const { return window_; }
		const Layout& layout() const { return *layout_; }
		Widget* value(const std::string& key) const;	// a slider row's value label
		Button* reset(const std::string& key) const;	// the row's reset button (shown while it differs from its default)
		const std::string& helpText() const;	// the help line: the hovered row's help (as of the last update)
		EditBox* search() const { return search_; }
		Button* findButton(bool next) const { return next ? next_ : prev_; }
		const std::string& query() const { return query_; }
		void find(const std::string& text);	// as typed in the find field: the best match; "" clears
		void step(bool forward);	// the next / previous match, wrapping
		size_t matches() const { return hits_.size(); }
		int hit() const { return hit_; }	// the match shown (0-based), -1: none
		const std::string& matchKey(size_t i) const { return hits_[i].key; }	// the ith match's key, in step order (i < matches())
		const std::string& countText() const;	// "3 of 7", "0 matches", "" with no text
		std::string highlighted() const;	// the highlighted row's key, "" when none
		size_t rowCount() const { return shownRows_.size(); }
		bool rowInView(const std::string& key) const;	// the row is whole inside the scrolled list (as of the last layout)
		size_t currentTab() const { return current_; }
		void reveal(const std::string& key);
		std::vector<std::string> groups() const;	// the shown tab's group headers, in order
		Label* needsLabel(const std::string& key) const;	// the row's needs note, null without one	// scroll the shown tab's row into view (at the next layout)

	private:
		struct Tab { std::string section, title; Button* button; };
		struct Hit { size_t tab; std::string key; int score; };
		struct Row { cfg::Info info; size_t tab; Widget* box; Widget* control; Label* value; Widget* swatch = nullptr; Widget* keys[2] = {}; Button* reset = nullptr; Label* name = nullptr; Label* needs = nullptr; };
		CaptionWindow* window_ = nullptr;
		Tabs* tabBar_ = nullptr;
		ScrollView* rows_ = nullptr;
		Label* help_ = nullptr;
		EditBox* search_ = nullptr;
		std::string query_;	// the find in effect
		std::vector<Hit> hits_;	// its matches, best first (score), ties in tab then row order
		int hit_ = -1;
		Label* count_ = nullptr;
		Button *prev_ = nullptr, *next_ = nullptr, *close_ = nullptr;
		std::vector<Tab> tabs_;
		std::vector<Row> shownRows_;
		size_t current_ = 0;
		uint64_t gen_ = 0;
		size_t optionCount_ = 0;
		void bind(Layout& l);	// the file's widgets, after each (re)build
		std::unique_ptr<Layout> layout_;
		void build();
		void select(size_t tab);
		void refresh();
		std::vector<cfg::Info> options(size_t tab) const;	// the tab's rows, in its order
		std::string label(const cfg::Info& info) const;
		void addRow(const cfg::Info& info, size_t tab);
		Label* needsNote(const cfg::Info& info, size_t t);	// the line under a row of tab t with a needs note
		std::string needsText(const std::string& needs, size_t t) const;
		void collect();	// hits_ from query_
		void go(int hit);	// show the match: its tab, scrolled to, highlighted
		void mark();	// the highlight on the shown rows
		void focusSearch();
	};
}
