#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/itemgrid.hpp"
#include "ui/widget.hpp"

// A production building's window (docs/UI.md), after Kenshi_GenericBuildingWindow.layout and kenshi-inspect's genbuilding.md /
// ui28_static.md part B: mines, wells, refineries, generators, batteries, rain collectors, farms, crafting benches and the item furnace
// (research benches and storage have windows of their own). Left edge at x 0, bottom edge at 470 of 720 lines (705 units), growing upwards;
// the size follows the columns. The title is the building's name in capitals, with a close X. Two columns, INPUT(S) | OUTPUT; with no input
// the OUTPUT column is alone, and the reverse. Each holds the item's name, a status line (crafting benches only: "Empty!" / "OK" / "Invalid
// item!"; the host's text and colour), its slot (an inventory section as ItemGrid: footprint x cells; an input takes only its ingredient, the
// output takes no drops but its items drag out) and a 112 x 16 (720p) Kenshi_ProgressBarFill: an input's buffered fraction of the unit being
// used, the output's progress on the current unit. A crafting bench adds the item in progress ("Nothing Queued") and QUEUE... over the columns;
// the item furnace adds Total Metals and the Active OFF / ON toggle under the output. No power, workers or "output full": those are the HUD's.
// Host-fed: one window shows one building; every action is an order and the next state shows its result.
namespace ui
{
	class Panel;
	class ProgressStrip;

	// The window's grids, as ItemGrid::tag and in actions; apart from Gear's kGear* numbers so a drag between the two names its source.
	enum BuildingGrid
	{
		kBuildingIn1 = 100,
		kBuildingIn2,
		kBuildingOut,
	};

	struct BuildingSlot	// a column's panel
	{
		std::string name;	// the item's: "Raw Iron"; a crafting bench's "-" when none
		std::string status;	// empty but on crafting benches
		std::string statusColour;	// a palette name ("HudGood"); empty: the panel's ink
		int cols = 0, rows = 0;	// the section, cells (the item's footprint)
		std::vector<int> accepts;	// GridItem kinds it takes; empty: any (the furnace)
		std::vector<GridItem> items;
		float bar = 0;	// 0..1
		bool operator==(const BuildingSlot&) const = default;
	};

	struct BuildingCrafting	// a crafting bench's panel over the columns
	{
		std::string name;	// the item in progress, else the host's "Nothing Queued"
		bool operator==(const BuildingCrafting&) const = default;
	};

	struct BuildingFurnace	// the item furnace's panel under the output
	{
		std::string metals;	// the host's: "4.5"
		bool ok = false;	// >= 1: HudGood, else HudBad
		bool active = false;	// ON / OFF
		bool operator==(const BuildingFurnace&) const = default;
	};

	struct BuildingWindowState
	{
		bool valid = false;	// false: the window is closed
		uint64_t id = 0;	// the building
		std::string name;	// the title (drawn in capitals)
		std::vector<BuildingSlot> inputs;	// 0 to 2
		std::optional<BuildingSlot> output;
		std::optional<BuildingCrafting> crafting;
		std::optional<BuildingFurnace> furnace;
		bool outputDropIn = false;	// the output slot takes a drop (a producer takes back its own item while it fits: the sim's call; kenshi-k16.19.7.2)
		bool operator==(const BuildingWindowState&) const = default;
	};

	struct BuildingWindowActions
	{
		// Dropped on one of this window's slots from fromGrid (a kBuilding* or Gear's kGear*) with its top-left at cell (x, y).
		std::function<void(uint64_t id, int fromGrid, const GridItem& item, int toGrid, int x, int y)> move;
		std::function<void(uint64_t id, int grid, const GridItem& item)> quickMove;	// a right-click: to the other open inventory (the host's)
		std::function<void(uint64_t id, int grid, const GridItem& item)> dropOutside;	// dragged out over the world
		std::function<void(uint64_t id)> queue;	// QUEUE...: the overview's CRAFTING page with this bench picked
		std::function<void(uint64_t id, bool on)> active;	// the furnace's Active toggle
		std::function<void(uint64_t id)> close;	// the close X / Escape
	};

	class BuildingWindow
	{
	public:
		explicit BuildingWindow(BuildingWindowActions actions);
		~BuildingWindow();
		void update(const BuildingWindowState& s);	// every frame, before ui::frame
		bool shown() const;
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		ItemGrid* grid(int g) const;	// a kBuilding* grid
		Widget* column(int g) const;	// its panel; hidden when absent
		Label* nameText(int g) const;
		Label* statusText(int g) const;
		Rect bar(int g) const;	// its progress strip, pixels
		Rect barFill(int g) const;	// the fill drawn last frame
		Label* title(bool output) const { return output ? outTitle_ : inTitle_; }
		Widget* craftingPanel() const;
		Label* craftingName() const { return craftName_; }
		Button* queueButton() const { return queue_; }
		Widget* furnacePanel() const;
		Label* metalsText() const { return metals_; }
		Button* activeButton() const { return active_; }

	private:
		BuildingWindowActions act_;
		BuildingWindowState last_;
		CaptionWindow* win_ = nullptr;
		Widget *inCol_ = nullptr, *outCol_ = nullptr;
		Label *inTitle_ = nullptr, *outTitle_ = nullptr;
		Panel* panels_[3] = {};
		Label* names_[3] = {};
		Label* status_[3] = {};
		ItemGrid* grids_[3] = {};
		ProgressStrip* bars_[3] = {};
		Panel *craft_ = nullptr, *furnace_ = nullptr;
		Label *craftName_ = nullptr, *metals_ = nullptr;
		Button *queue_ = nullptr, *active_ = nullptr;
		Widget& addSlot(Widget& col, int g);
		void fill(int g, const BuildingSlot* s);
	};
}
