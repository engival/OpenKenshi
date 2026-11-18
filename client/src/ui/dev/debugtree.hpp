#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The sim's DEBUG VIEW as a tree (docs/UI.md; client coordinator 2026-11-05): a resizable window, top right, the host's rows (flat pre-order,
// children at depth + 1) under a header strip from client/data/ui/debugtree.layout (tick, filter, Auto, REFRESH, COPY; the caption's X
// closes). A node with children has a toggle; depth 0 starts open, deeper closed; the open state is kept by key path across refreshes;
// Shift+click opens / closes a whole subtree. The filter keeps the rows whose key or value holds it (any case) and their ancestors, open.
// A title "{#rrggbb}Name" shows a colour swatch before the name. The host sends the queries and owns the clipboard.
namespace ui
{
	struct DebugTreeRow
	{
		int depth = 0;
		std::string key, value;
		bool operator==(const DebugTreeRow&) const = default;
	};

	struct DebugTreeState
	{
		std::string title;	// "{#rrggbb}" prefix: a swatch
		uint64_t tick = 0;
		std::vector<DebugTreeRow> rows;
		bool autoOn = false;
		bool operator==(const DebugTreeState&) const = default;
	};

	struct DebugTreeActions
	{
		std::function<void()> refresh;
		std::function<void(bool)> setAuto;
		std::function<void()> close;	// the X or Escape (the window hides itself first)
		std::function<void(const std::string&)> copy;	// the visible rows as indented "key: value" lines, for the clipboard
	};

	class DebugTree
	{
	public:
		explicit DebugTree(DebugTreeActions actions = {});
		~DebugTree();
		void update(const DebugTreeState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		std::string text() const;	// what COPY gives
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		size_t visibleRows() const { return shown_.size(); }
		const std::string& path(size_t i) const { return paths_[shown_[i]]; }	// visible row i's key path
		Widget* toggle(size_t i) const;	// null: a leaf
		Widget* row(size_t i) const { return i < rowWidgets_.size() ? rowWidgets_[i] : nullptr; }
		Widget* swatch() const { return swatch_; }
		EditBox* filter() const { return filter_; }
		Button* refreshButton() const { return refresh_; }
		Button* copyButton() const { return copy_; }
		Button* autoBox() const { return auto_; }

	private:
		class Toggle;
		void bind(Layout& l);
		void rebuild();	// shown_ and the row widgets from last_, open_ and the filter
		void flip(size_t row, bool subtree);
		bool open(size_t row) const;
		CaptionWindow* win_ = nullptr;
		Widget* swatch_ = nullptr;
		std::unique_ptr<Layout> layout_;
		Label* tick_ = nullptr;
		EditBox* filter_ = nullptr;
		Button *auto_ = nullptr, *refresh_ = nullptr, *copy_ = nullptr;
		ScrollView* list_ = nullptr;
		DebugTreeState last_;
		DebugTreeActions act_;
		std::vector<std::string> paths_;	// per row
		std::vector<int> parent_;	// per row, -1 at the top
		std::vector<size_t> shown_;	// visible rows' indices
		std::vector<Widget*> rowWidgets_, toggles_;
		std::map<std::string, bool> open_;	// set by the user; others: depth 0 open
		std::string filterText_;
		bool dirty_ = true;
	};
}
