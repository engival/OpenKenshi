#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The character stats window (docs/UI.md), after the original's Kenshi_StatsWindow (kenshi-inspect findings/ui_captures/stats_window.md), layout
// ours: ATTRIBUTES top left, SKILLS in four columns of "-Group-" headed rows beside it, DESCRIPTION under both, DERIVED STATISTICS down the right
// (the original's layout has the column but clips it off; ours shows it). Hovering a row, else the last clicked one, fills the description: name,
// "Stats Affected:" and "Ways To Train:" lines on the left, the "Next Level:" bar and the description text on the right. Grouping, order and all
// text are the host's. Like the Gear it knows nothing about SIMLINK: the host fills a StatsState every frame.
namespace ui
{
	struct StatRow
	{
		std::string name;
		float level = 0;	// shown as a whole number
		float xp = 0;		// 0..1 towards the next level: the description's Next Level bar
		float base = -1;	// the level before modifiers; < 0 = the same. Otherwise the level is StatUp / StatDown, the description gives "18 (+2)"
		std::vector<std::string> affects = {}, training = {};	// "Stats Affected:" / "Ways To Train:" lines, without the dash
		std::string description = {};	// wrapped
		bool operator==(const StatRow&) const = default;
	};

	struct StatGroup
	{
		std::string title;	// shown "-Title-" over the rows; empty: none
		int column = 0;		// 0..3, groups stack in the order given
		std::vector<StatRow> rows;
		bool operator==(const StatGroup&) const = default;
	};

	struct StatsState
	{
		bool valid = false;	// false: nothing to show yet
		std::string name;	// the caption; empty: CHARACTER STATS
		std::vector<StatRow> attributes;
		std::vector<StatGroup> skills;
		std::string derived;	// rich text (layoutRich: "label\tvalue" rows, "{Colour}", wrapped paragraphs), as in the tooltips
		bool operator==(const StatsState&) const = default;
	};

	struct StatsActions
	{
		std::function<void()> close;	// the close button or Escape (the window hides itself first)
	};

	class Stats
	{
	public:
		explicit Stats(StatsActions actions);
		~Stats();
		void update(const StatsState& s);	// every frame, before ui::frame (cheap when nothing changed; values change in place)
		bool shown() const;
		void show(bool on);	// opening clears the clicked row
		bool pinned() const;	// the caption's pin: Escape leaves the window open. What else it means (keeping the window on its character when the
								// selection changes, surviving a "close all") is the host's.
		// For scripted tests.
		Widget* window() const;
		Widget* row(const std::string& name) const;	// an attribute or skill row; null when the state has none of that name
		std::string levelText(const std::string& name) const;	// the row's shown level
		std::string described() const;	// the row the description shows, empty for none
		Widget* derived() const;	// a RichText
		Widget* about() const;	// the description's left RichText (name, Stats Affected, Ways To Train)
		Widget* text() const;	// its right RichText (Description:)

	private:
		class Line;
		class NextLevel;
		CaptionWindow* win_ = nullptr;
		Label* empty_ = nullptr;
		Widget* body_ = nullptr;
		Panel *attrInset_ = nullptr, *skillInset_ = nullptr, *descInset_ = nullptr, *derivedInset_ = nullptr;
		Box* attrRows_ = nullptr;
		Box* left_ = nullptr;	// attributes and skills over the description
		Label* derivedHead_ = nullptr;
		Box* columns_[4] = {};
		RichText *derived_ = nullptr, *about_ = nullptr, *text_ = nullptr;
		NextLevel* next_ = nullptr;
		std::vector<Line*> lines_;	// attributes, then the skills in state order
		std::vector<StatRow> rows_;	// the same order
		int picked_ = -1, shown_ = -1;	// the clicked row, the described one
		StatsActions act_;
		StatsState last_;
		void rebuild();
		void refresh();	// values into the existing rows
		void describe(int i);
		void fit();		// the insets' heights: the top row's match, the derived column runs the full height
	};
}
