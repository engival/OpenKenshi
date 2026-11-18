#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The crafting page of the overview window (docs/UI.md), after Kenshi_OverviewWindow.layout's CraftTab (the boxes, as fractions of the page)
// and kenshi-inspect's crafting.md / craft27_static.md (the empty tab live, the rest static). The caption is "CRAFTING". STATIONS: the
// faction's crafting benches, the picked one gold (red with its status as the tooltip when it cannot work: ours). ITEMS: the picked bench's
// unlocked recipes as buttons in the Main colour, the name only; a click appends one copy (no material check: the bench draws them as it
// works); locked recipes are not sent; none: the host's empty text. QUEUE: the bench's queue in order as Kenshi_ItemListItem rows, each with
// the host's progress (only the front is worked); the X takes one out (a right-click too, ours), a drag moves it; REPEAT QUEUE under it, one
// tick per bench. DESCRIPTION, two panels of host lines: the hovered recipe (its stats and ingredients left, craft time and costs right), or the
// hovered queue row (the recipe's left, the row's state, ETA, smith, predicted result and crit chance right), else the last clicked one while
// listed or queued, else the queue's first. Host-fed like Research: the UI computes nothing; every action is an order and the next state
// shows its result.
namespace ui
{
	class Overview;
	class ButtonRows;

	struct CraftLine	// a description line, "label\tvalue"; the host's text ("Estimated Craft Time:" "3 hrs")
	{
		std::string label, value;	// value empty: the label alone
		std::string colour;	// a palette name for the value ("Bad", "ItemGrade3"); empty: the panel's ink
		bool operator==(const CraftLine&) const = default;
	};

	struct CraftRecipe
	{
		std::string id, name;	// name: "Plated Vest"; a weapon "Name, Grade"
		std::vector<CraftLine> stats;	// the left panel: the item's stats and "Ingredients"
		std::vector<CraftLine> info;	// the right panel: "Estimated Craft Time:", the costs
		bool operator==(const CraftRecipe&) const = default;
	};

	struct CraftQueued
	{
		std::string recipe;	// a CraftRecipe id of the same bench
		float progress = 0;	// 0..1, the row's bar (the game works only the front)
		std::vector<CraftLine> info;	// the right panel while hovered: "State:", "ETA:", "-Current Smith:", "-Predicted Result:", ...
		bool operator==(const CraftQueued&) const = default;
	};

	struct CraftStation
	{
		std::string id, name;
		std::string status;	// the host's: "No power", "No workers" (?); empty: working
		bool ok = true;	// false: the row red
		std::vector<CraftRecipe> recipes;
		std::vector<CraftQueued> queue;
		bool repeat = false;	// REPEAT QUEUE
		bool operator==(const CraftStation&) const = default;
	};

	struct CraftingState
	{
		std::vector<CraftStation> stations;
		std::string station;	// the picked one's id
		std::string empty;	// ITEMS's text when the picked bench lists nothing ("[No items to craft yet ...]")
		bool operator==(const CraftingState&) const = default;
	};

	struct CraftingActions
	{
		std::function<void(const std::string& id)> station;	// a STATIONS row
		std::function<void(const std::string& station, const std::string& recipe)> queue;	// a click on a recipe: one more at the back
		std::function<void(const std::string& station, int index)> unqueue;	// a queue row's X, or a right-click on it
		std::function<void(const std::string& station, int from, int to)> move;	// a queue row dropped on another
		std::function<void(const std::string& station, bool on)> repeat;	// REPEAT QUEUE
		std::function<void()> close;	// the overview closed or left the page while it showed crafting
	};

	class Crafting
	{
	public:
		Crafting(Overview& home, CraftingActions actions);	// its content goes on home's CRAFTING page
		~Crafting();
		void update(const CraftingState& s);	// every frame, before ui::frame (the description follows the hover)
		bool shown() const;
		void show(bool on);
		// For scripted tests.
		List* stationList() const { return stations_; }
		Widget* items() const;
		Widget* queueList() const;
		Button* repeatButton() const { return repeat_; }
		Rect itemRow(size_t i) const;	// pixels; the i-th recipe's button
		Rect queueRow(size_t i) const;
		std::vector<std::string> listed() const;	// the recipes' ids, in order
		std::string described() const { return described_; }	// the recipe the description shows; empty: none
		int describedRow() const { return describedRow_; }	// the queue row it shows; -1: a recipe of ITEMS
		Label* emptyText() const { return empty_; }
		RichText* details() const { return details_; }
		RichText* description() const { return text_; }

	private:
		Overview& home_;
		CraftingActions act_;
		CraftingState last_;
		bool wasShown_ = false;
		List* stations_ = nullptr;
		ButtonRows *items_ = nullptr, *queue_ = nullptr;
		Button* repeat_ = nullptr;
		Label* empty_ = nullptr;
		RichText *details_ = nullptr, *text_ = nullptr;
		std::string clicked_, described_;
		int clickedRow_ = -1, describedRow_ = -1;	// a queue row; -1: ITEMS
		const CraftStation* station() const;	// the picked one; null: none
		const CraftRecipe* recipe(const std::string& id) const;	// of the picked station
		void refresh();
		void describe(const std::string& id, int row);	// row: a queue row, -1: ITEMS
	};
}
