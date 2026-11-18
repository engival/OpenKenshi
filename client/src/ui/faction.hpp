#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The faction page of the overview window (docs/UI.md), after Kenshi_OverviewWindow.layout's FactionTab (the boxes, as fractions of the
// page), Kenshi_FactionRelationsLine.layout (a row) and the darkui capture (findings/ui_captures/darkui_faction_0926.jpg). RELATIONS: a row per
// faction in the host's order, the name left and the value right (red below zero), a bar from the row's middle: LifeBarHurt leftwards below
// zero, LifeBar rightwards above, |value| / 100 of the half; the exact value the tooltip. FACTION NAME: the player faction's, an edit box renamed when committed (Enter or focus loss; order 85 faction_rename).
// INFORMATION: the host's rich text ("---Upcoming Events---"). Host-fed like Squads: the UI computes nothing.
namespace ui
{
	class Overview;
	class RelationRows;

	struct FactionRelation
	{
		std::string id, name;
		float value = 0;	// -100..100, the sim's (FACTION_RELATIONS): the row shows it cut toward zero, the tooltip (ours) to two places
		bool operator==(const FactionRelation&) const = default;
	};

	struct FactionState
	{
		std::string caption;	// the window's while on this page; empty: "FACTION"
		std::string name;	// the player faction's
		std::vector<FactionRelation> relations;
		std::string info;	// rich text, INFORMATION
		bool operator==(const FactionState&) const = default;
	};

	struct FactionActions
	{
		std::function<void(const std::string& name)> rename;	// FACTION NAME committed: Enter, or the focus leaving it changed (Escape restores)
		std::function<void()> close;	// the overview closed or left the page while it showed faction
	};

	class Faction
	{
	public:
		Faction(Overview& home, FactionActions actions);	// its content goes on home's FACTION page
		~Faction();
		void update(const FactionState& s);	// every frame, before ui::frame
		bool shown() const;
		void show(bool on);
		// For scripted tests.
		Widget* relations() const;
		Rect relationRow(size_t i) const;	// pixels; the i-th row's box
		Rect relationBar(size_t i) const;	// pixels; its bar (w 0: none)
		std::string relationText(size_t i) const;	// the value as drawn
		Color relationColour(size_t i) const;
		EditBox* nameBox() const { return name_; }
		RichText* info() const { return info_; }

	private:
		Overview& home_;
		FactionActions act_;
		FactionState last_;
		bool wasShown_ = false;
		RelationRows* rows_ = nullptr;
		EditBox* name_ = nullptr;
		RichText* info_ = nullptr;
		void refresh();
	};
}
