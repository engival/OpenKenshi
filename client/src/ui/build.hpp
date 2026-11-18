#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The build window (docs/UI.md), after Kenshi_BuildWindow.layout and kinspect's capture (findings/ui_captures/build.md; boxes x 1.5):
//  - the main panel, bottom left (1068 x 304.5): CATEGORIES and BUILDINGS lists (plain text, the picked row light grey, no hover); the
//    picked building's name, between < > arrows when its group has variants; its picture, its stats (label / value lines; material lines
//    indented) and its description.
//  - while placing (the host's ghost; placement itself is the host's): the button panel bottom right (CONFIRM, UNDO, the hammer) and the
//    COMMANDS panel under it (the host's key help).
//  - while shown, the HUD message box (ui::message) sits over the main panel, where the original moves its message roller: the host's
//    refusals on a failed placement click.
// Host-fed like the others: the UI computes nothing; every click is an action and the next state shows its result.
namespace ui
{
	struct BuildEntry	// a category or a building group; the original lists only unlocked ones, plain
	{
		std::string id, name;
		std::string selectSound;	// a building: its record's "select sound", the Buildings_Click switch value of Building_Click; empty = none
		bool operator==(const BuildEntry&) const = default;
	};

	struct BuildLine
	{
		std::string label, value;
		Color colour = { 0, 0, 0, 0 };	// the whole line; alpha 0 = the inset's ink (the host's: style BuildMaterial / BuildPowerOut / BuildPowerUse)
		bool indent = false;	// a material line, under "Materials cost:"
		bool operator==(const BuildLine&) const = default;
	};

	struct BuildState
	{
		bool chooser = true;	// the main panel (the lists and the picked building)
		bool placing = false;	// the button and commands panels
		std::vector<BuildEntry> categories, buildings;	// buildings: the current category's
		std::string category, picked;	// ids; empty: none
		std::string title;	// BuildingName, between the arrows
		bool variants = false;	// the < > arrows: the group has two or more unlocked members
		bool canUndo = false;	// UNDO enabled: something placed
		PortraitRef image = {};	// BuildingImage; tex 0: none
		std::vector<BuildLine> stats;
		std::string description;	// rich text (layoutRich)
		std::string commands;	// COMMANDS: the key help while placing (rich text; the original: labels style BuildLabel, keys BuildKey)
		bool operator==(const BuildState&) const = default;
	};

	struct BuildActions
	{
		std::function<void(const std::string& id)> category, pick;
		std::function<void(int d)> step;	// the < (-1) / > (+1) arrows
		std::function<void()> confirm, undo, build;	// CONFIRM, UNDO, the hammer
		std::function<void()> close;	// Escape on the main panel, or the host's own; the host decides what it cancels
	};

	class Build
	{
	public:
		static constexpr float kW = 1068, kH = 304.5f;	// MainPanel: 712 x 203 at 720
		static constexpr float kLineH = 20, kIndent = 18;	// stats lines (13.5 px at 720); a material line's indent
		explicit Build(BuildActions actions = {});
		~Build();
		void update(const BuildState& s);	// every frame while shown (cheap when nothing changed)
		void show(bool on);	// the main panel takes the keyboard when it shows; the message box moves over it until hidden
		bool shown() const;
		// For scripted tests.
		Widget* mainPanel() const;
		Widget* buttonPanel() const;
		Widget* commandsPanel() const;
		List* categoryList() const { return cats_; }
		List* buildingList() const { return list_; }
		Label* title() const { return title_; }
		Button* prevButton() const { return prev_; }
		Button* nextButton() const { return next_; }
		Button* confirmButton() const { return confirm_; }
		Button* undoButton() const { return undo_; }
		Button* buildButton() const { return build_; }
		Label* statLabel(size_t i) const;
		Label* statValue(size_t i) const;
		RichText* description() const { return info_; }
		RichText* commands() const { return commands_; }

	private:
		Window* main_ = nullptr;
		Panel *buttons_ = nullptr, *commandsBox_ = nullptr;
		List *cats_ = nullptr, *list_ = nullptr;
		Label* title_ = nullptr;
		Rect messageRect_;	// the message box's own place, while ours
		Widget::Align messageH_ = Widget::Align::kStart, messageV_ = Widget::Align::kStart;
		Button *prev_ = nullptr, *next_ = nullptr, *confirm_ = nullptr, *undo_ = nullptr, *build_ = nullptr;
		TextureImage* image_ = nullptr;
		Box* stats_ = nullptr;
		std::vector<std::pair<Label*, Label*>> lines_;
		RichText *info_ = nullptr, *commands_ = nullptr;
		ScrollView* infoScroll_ = nullptr;
		bool shown_ = false;
		BuildState last_;
		BuildActions act_;
		void refresh();
	};
}
