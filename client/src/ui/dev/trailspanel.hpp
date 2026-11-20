#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/widget.hpp"

// The trails panel (kenshi-k16.3.11; docs/UI.md "trailspanel.*"): a debug window (--debug, the host's) that drives the host's
// Trails (client/src/frame/trails.hpp, kenshi-k16.2.73) without a script: on / off, all or one character, the last X seconds (at most the host's cap;
// 0 = the cap, LAST N S; kenshi-k16.2.106), clear; one row per character with its track colour, points and pops (pops > 0 in the warning ink); the host's
// status line at the bottom. The body is client/data/ui/trailspanel.layout. Host-fed: the state in every frame while shown, actions out.
namespace ui
{
	struct TrailsChar
	{
		uint64_t id = 0;
		std::string name;
		std::string colour;	// "#rrggbb": the character's track hue (the host's, green -> violet); empty: no swatch
		uint32_t points = 0;	// kept points, all four tracks
		uint32_t pops = 0;	// frame moves over the pop rule (the warning-coloured segments)
		bool operator==(const TrailsChar&) const = default;
	};

	struct TrailsPanelState
	{
		bool on = false;
		uint64_t pick = 0;	// 0: all characters
		float window = 0;	// seconds shown; 0: the cap
		float cap = 30;	// the host's debug.trails_seconds: the most it draws (older points go to the dump)
		std::vector<TrailsChar> chars;
		std::string status;	// the host's one-line readout (Trails::status)
		bool operator==(const TrailsPanelState&) const = default;
	};

	struct TrailsPanelActions
	{
		std::function<void(bool)> setOn;
		std::function<void(uint64_t)> pick;	// a character's id, 0 = all
		std::function<void(float)> window;	// seconds, 0 = the cap
		std::function<void()> clear;
		std::function<void()> close;	// the X or Escape (the window hides itself first)
	};

	class TrailsPanel
	{
	public:
		explicit TrailsPanel(TrailsPanelActions actions = {});
		~TrailsPanel();
		void update(const TrailsPanelState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);
		bool shown() const;
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		Button* onBox() const { return on_; }
		EditBox* windowBox() const { return seconds_; }
		Button* capButton() const { return cap_; }	// LAST N S: window 0
		Button* clearButton() const { return clear_; }
		Label* statusLabel() const { return status_; }
		ScrollView* list() const { return list_; }
		size_t rows() const { return rows_.size(); }	// the All row + one per character
		Widget* row(size_t i) const;	// 0: All
		Label* popsLabel(size_t i) const { return i < pops_.size() ? pops_[i] : nullptr; }	// a character row's pops (i >= 1); null on All
		Widget* swatch(size_t i) const { return i < swatches_.size() ? swatches_[i] : nullptr; }	// null: All, or no colour

	private:
		class Row;
		void bind(Layout& l);
		void rebuild();	// the rows from last_: only when the row set (ids, names, colours) changes
		void refresh();	// the counts and the pick into the kept rows
		void picked(uint64_t id);	// a row click: that one, or all again when it was the pick
		void commitWindow(const std::string& text);
		void capText();	// LAST N S and its tip from last_.cap
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		Button *on_ = nullptr, *cap_ = nullptr, *clear_ = nullptr;
		EditBox* seconds_ = nullptr;
		Label* status_ = nullptr;
		ScrollView* list_ = nullptr;
		TrailsPanelState last_;
		TrailsPanelActions act_;
		std::vector<Row*> rows_;
		std::vector<Widget*> swatches_;
		std::vector<Label*> pops_;
		bool dirty_ = true;
	};

	std::string trailsSeconds(float s);	// the Window box's text: "0" for the cap, else the seconds without trailing zeros ("12.5")
}
