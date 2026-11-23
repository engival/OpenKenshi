#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The roads panel (kenshi-nken; docs/UI.md "roadspanel.*"): the debug window of the road overlay (debug.roads, the host's; the editor kenshi-k16.2.513).
// The suspect list (one row per suspect marker: a click selects it; Rank, Extra and Mark headers sort; a filter by mark and one by kind), then the info block of the
// selected marker (any marker, not only a suspect) with OK / UNSURE / BAD / CLEAR and COPY, then the host's status line. The body is
// client/data/ui/roadspanel.layout. Host-fed: the state in every frame while shown, actions out. Sorting and the filter are the panel's own view.
namespace ui
{
	enum RoadMark : int { kRoadUnmarked = 0, kRoadOk = 1, kRoadUnsure = 2, kRoadBad = 3 };

	struct RoadsVec
	{
		float x = 0, y = 0, z = 0;
		bool operator==(const RoadsVec&) const = default;
	};

	struct RoadsSuspect
	{
		int rank = 0;	// 1: the worst
		std::string road;	// display text: today the road's index and its junctions, later its string id
		int marker = 0, count = 0;	// the marker's index on the road, the road's marker count
		RoadsVec pos;
		float ratio = 0, turn = 0, extra = 0;	// the host's suspect numbers
		int mark = kRoadUnmarked;
		std::string kind;	// the host's suspect kind ("reversal", "overlap", ...): the Kind column and filter
		bool operator==(const RoadsSuspect&) const = default;
	};

	// The selected marker: any marker, a suspect or not.
	struct RoadsInfo
	{
		bool valid = false;	// false: nothing selected
		std::string road;
		int marker = 0, count = 0;
		RoadsVec pos;
		bool hasPrev = false, hasNext = false;	// false: a road end on that side
		RoadsVec prev, next;
		int suspect = -1;	// its index into RoadsPanelState::suspects, -1: not a suspect
		std::string junction;	// a road end: its junction's id; empty: not a road end
		int degree = 0;	// the junction's road count
		int mark = kRoadUnmarked;
		std::string roads;	// the host's text: every road through the marker
		bool operator==(const RoadsInfo&) const = default;
	};

	struct RoadsPanelState
	{
		std::vector<RoadsSuspect> suspects;
		int selected = -1;	// an index into suspects, -1: none (the info may still hold a non-suspect marker)
		RoadsInfo info;
		std::string status;	// a line of host text
		bool operator==(const RoadsPanelState&) const = default;
	};

	struct RoadsPanelActions
	{
		std::function<void(int)> select;	// a row click: the index into suspects
		std::function<void(int)> mark;	// the info block's buttons: a RoadMark (CLEAR: kRoadUnmarked)
		std::function<void()> copy;	// COPY: the host puts "x y z" of the selection on the clipboard
		std::function<void()> close;	// the X or Escape (the window hides itself first)
	};

	class RoadsPanel
	{
	public:
		enum class Sort { kRank, kExtra, kMark };
		enum Filter : int { kAll = 0, kUnmarked, kOk, kUnsure, kBad };

		explicit RoadsPanel(RoadsPanelActions actions = {});
		~RoadsPanel();
		void update(const RoadsPanelState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		void sortBy(Sort by);	// a header click: again on the same column flips the order
		void filter(int f);	// a Filter
		void filterKind(const std::string& kind);	// empty: all kinds; a kind not in the data: all
		const std::string& kindFilter() const { return kindOn_; }
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		ScrollView* list() const { return list_; }
		Tabs* filterTabs() const { return filter_; }
		Tabs* kindTabs() const { return kindTabs_; }	// ALL, then each kind in the data (first seen first), upper case
		size_t rows() const { return rows_.size(); }	// the shown rows, in view order
		Widget* row(size_t i) const;
		int rowSuspect(size_t i) const;	// the suspect index a shown row stands for, -1: none
		Label* markLabel(size_t i) const;	// a shown row's mark cell
		Widget* header(Sort s) const;	// a sortable header cell
		Sort sort() const { return sort_; }
		bool descending() const { return desc_; }
		Button* markButton(int mark) const { return mark >= 0 && mark < 4 ? marks_[mark] : nullptr; }	// kRoadUnmarked: CLEAR
		Button* copyButton() const { return copy_; }
		Label* infoLabel(const std::string& id) const;	// "road", "roads", "marker", "pos", "prev", "next", "suspect", "junction", "mark"
		Label* statusLabel() const { return status_; }

	private:
		class Row;
		class Head;
		void bind(Layout& l);
		void rebuild();	// the header and rows from last_, the sort and the filter
		void refresh();	// the selection into the kept rows
		void fillInfo();
		void fillKinds();	// the kind tabs from last_ (when the kinds change)
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		Widget* head_ = nullptr;
		ScrollView* list_ = nullptr;
		Tabs* filter_ = nullptr;
		Tabs* kindTabs_ = nullptr;
		std::vector<std::string> kinds_;	// the kind tabs after ALL
		std::string kindOn_;	// empty: all
		Button* marks_[4] = {};
		Button* copy_ = nullptr;
		Label* status_ = nullptr;
		std::vector<std::pair<std::string, Label*>> info_;
		RoadsPanelState last_;
		RoadsPanelActions act_;
		std::vector<Row*> rows_;
		Head* heads_[3] = {};
		Sort sort_ = Sort::kRank;
		bool desc_ = false;
		int filterOn_ = kAll;
		bool dirty_ = true;
	};

	const char* roadMarkName(int mark);	// "-", "OK", "UNSURE", "BAD"
	Color roadMarkColour(int mark, bool inset);	// inset: on the list (the RoadMark* names, darker on the original's parchment); else on the window
	std::string roadsVec(const RoadsVec& v);	// "x y z", one decimal
	std::string roadsTurn(float deg);	// "162°"
	std::string roadsExtra(float u);	// "318 u" (world units)
}
