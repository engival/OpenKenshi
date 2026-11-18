#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The research page of the overview window (docs/UI.md), after Kenshi_OverviewWindow.layout's ResearchTab (the boxes, as fractions of the
// page) and the darkui capture (findings/ui_captures/darkui_research_0925.jpg: the contents). TYPES: the categories, the picked one gold.
// kenshi-inspect's research.md and research25_static.md: the rows, the colours, the description lines. BLUEPRINTS: a 185 x 20 button (720p) per
// entry of the picked type that is neither done nor queued, the name left and the time right, its kept progress a bar, red when it cannot start
// (a click does nothing; the tooltip, ours, says why). QUEUE: Kenshi_ItemListItem rows in order, each with its progress; the X takes one out (a
// right-click too, ours), a drag moves it; the bench message over the box (the host sends it only without a bench). DESCRIPTION: the hovered
// blueprint, else the last clicked entry while listed or queued, else the queue's first: name and level, ETA, the warning, costs (one line
// each), requirements (one line, ", "-joined: done grey, queued good, unmet bad; "-" for none) and unlocks on the left, the description on the right. The window's caption is the host's (research level and rate). Host-fed like Squads: the
// UI computes nothing; every action is an order and the next state shows its result.
namespace ui
{
	class Overview;
	class ButtonRows;

	struct ResearchCategory
	{
		std::string id, name;
		bool operator==(const ResearchCategory&) const = default;
	};

	struct ResearchNeed	// a cost ("Book x1", "Paid") or a requirement (another research); the host's text
	{
		enum class State { kMet, kQueued, kMissing };	// a cost: in stock / -, missing; a requirement: done, queued, unmet
		std::string text;
		State state = State::kMet;	// kMissing: red, and the blueprint cannot start
		bool operator==(const ResearchNeed&) const = default;
	};

	struct ResearchUnlock	// a line of what it gives: "New Building:" "Mounted Crossbow mkII [light]"
	{
		std::string label, text;
		bool operator==(const ResearchUnlock&) const = default;
	};

	struct ResearchEntry
	{
		enum class State { kAvailable, kLocked, kInProgress, kDone };
		std::string id, name, category;	// category: a ResearchCategory id
		int level = 1;
		std::string time, eta;	// the host's text: "<1 hr", "2 hrs"; "<1 hr (at 1x rate)"
		std::vector<ResearchNeed> costs, needs;
		std::vector<ResearchUnlock> unlocks;
		std::string description;	// rich text
		State state = State::kAvailable;	// kLocked: red, no click; kDone: not listed
		float progress = 0;	// 0..1, the row's bar, listed (kept from an earlier queueing) or queued
		std::string warning = {};	// a line of its own under ETA in the missing colour ("Needs research bench level 4"); empty: none
		bool operator==(const ResearchEntry&) const = default;
	};

	struct ResearchState
	{
		std::string caption;	// the window's while on this page; empty: "RESEARCH"
		std::vector<ResearchCategory> categories;
		std::string category;	// the picked one's id
		std::vector<ResearchEntry> entries;	// all of them; the page lists the picked category's
		std::vector<std::string> queue;	// entry ids, first = being researched
		std::string benchMessage;	// over the queue box, half alpha; empty: none
		bool operator==(const ResearchState&) const = default;
	};

	struct ResearchActions
	{
		std::function<void(const std::string& id)> category;	// a TYPES row
		std::function<void(const std::string& id)> queue;	// a click on a blueprint that can start
		std::function<void(const std::string& id)> unqueue;	// a right-click on a queue row
		std::function<void(const std::string& id, int index)> move;	// a queue row dropped on another: it ends at index
		std::function<void()> close;	// the overview closed or left the page while it showed research
	};

	class Research
	{
	public:
		Research(Overview& home, ResearchActions actions);	// its content goes on home's RESEARCH page
		~Research();
		void update(const ResearchState& s);	// every frame, before ui::frame (the description follows the hover)
		bool shown() const;
		void show(bool on);
		// For scripted tests.
		List* typeList() const { return types_; }
		Widget* blueprints() const;
		Widget* queueList() const;
		Rect blueprintRow(size_t i) const;	// pixels; the i-th listed blueprint's button
		Rect queueRow(size_t i) const;
		std::vector<std::string> listed() const;	// the blueprints' ids, in order
		std::string described() const { return described_; }	// the entry the description shows; empty: none
		RichText* details() const { return details_; }
		RichText* description() const { return text_; }
		Label* benchMessage() const { return bench_; }

	private:
		Overview& home_;
		ResearchActions act_;
		ResearchState last_;
		bool wasShown_ = false;
		List* types_ = nullptr;
		ButtonRows *blue_ = nullptr, *queue_ = nullptr;
		Label* bench_ = nullptr;
		RichText *details_ = nullptr, *text_ = nullptr;
		std::string clicked_, described_;
		const ResearchEntry* entry(const std::string& id) const;
		void refresh();
		void describe(const std::string& id);
	};
}
