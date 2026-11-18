#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "ui/gear.hpp"

// The item windows (docs/UI.md "Item windows", kenshi-k16.20.13.1.1): several character windows (ui::Gear, with their backpack and robotics)
// and several container windows (ui::ContainerWindow: boxes, a body's stock, a trader's) open at once, each keyed by its owner's id. The first
// container docks beside the first character window as the single Gear always did; the rest cascade from it and move on their own. Every window
// shows a pin and a target toggle (ours: the original opens one container and pins no world container, kenshi-rwh.78). Host-fed as ever: the
// state lists what is open, the actions name grids as GridRef{owner, grid}.
namespace ui
{
	struct GridRef
	{
		uint64_t owner = 0;	// the window's owner; 0: not an item window's grid
		int grid = 0;		// a kGear* number: a character window's grid, or kGearContainer for a container window's
		bool operator==(const GridRef&) const = default;
	};

	// What a right click on an item would do, the host's answer for the item under the pointer with the modifiers held (kenshi-k16.20.13.1.2):
	// drawn as a badge by the pointer; kMenu (ambiguous: several destinations, none marked) opens the item menu instead of a quick click.
	enum class RightVerb { kNone, kBuy, kSell, kTransfer, kMenu };

	// A row of the item menu (ours, flat drawn): the label left, its binding right and dimmed ("Ctrl+Right click"); a disabled row greyed.
	struct ItemMenuRow
	{
		std::string label;
		std::string binding = {};
		bool enabled = true;
		int action = 0;	// handed back to pick()
	};

	// The item menu: a title row (the item's name), then the host's rows with their bindings.
	class ItemMenu : public Menu
	{
	public:
		ItemMenu(const std::string& title, const std::vector<ItemMenuRow>& rows, std::function<void(int action)> onPick);
		std::vector<std::string> bindings;	// per item (the title's empty)
		Size preferred() const override;
		Rect row(size_t i) const;	// item i's row, pixels (scripted tests)

	protected:
		void paint() const override;
	};

	struct ItemWindowsState
	{
		std::vector<GearState> characters;	// one character window each, in this order (two: the first docks left, the second right; more cascade); their container fields unused
		std::vector<ContainerState> containers;	// one window each; at most one trading (the host's rule)
		uint64_t target = 0;	// the marked window's owner (its target toggle drawn checked); 0: none
		bool operator==(const ItemWindowsState&) const = default;
	};

	struct ItemWindowsActions
	{
		// Dragged from one grid to another (or within one) with its top-left at cell (x, y); item.count is the count carried (kenshi-k16.20.12.1).
		std::function<void(GridRef from, const GridItem& item, GridRef to, int x, int y)> move;
		std::function<void(GridRef at, const GridItem& item)> activate;	// double-click
		std::function<void(GridRef at, const GridItem& item, uint32_t mods)> quick;	// a right click (the host routes it: the marked window, else ...)
		std::function<void(GridRef at, const GridItem& item)> moveAll;	// Ctrl + right click (Ctrl wins over Shift); unset: a quick right click
		std::function<void(GridRef at, const GridItem& item)> dropOutside;	// dragged out over the world
		std::function<void(GridRef at)> arrange;	// ARRANGE
		std::function<void(uint64_t owner)> mark;	// the target toggle: its window's owner, or 0 when clicked on the marked one
		std::function<void(uint64_t owner)> close;	// a window's close button / Escape (not while pinned)
		std::function<void(uint64_t owner, bool on)> pin;	// a window's pin toggled
		std::function<void(uint64_t owner)> showResearch;	// a research container's SHOW RESEARCH
		// Queries (kenshi-k16.20.13.1.2), asked at once: the verb of a right click (on hover, at most once a frame, and on a quick right click);
		// the menu's rows when it opens; the row picked. verb unset: kNone (no badge, every quick right click goes to quick()).
		std::function<RightVerb(GridRef at, const GridItem& item, uint32_t mods)> verb;
		std::function<std::vector<ItemMenuRow>(GridRef at, const GridItem& item)> menu;
		std::function<void(GridRef at, const GridItem& item, int action)> pick;
	};

	class ItemWindows
	{
	public:
		static constexpr float kCascade = 30;	// units, right and down, per container window after the docked one
		static constexpr int kTagStep = 1000;	// each window's grids are tagged from its own multiple of this

		float holdTime = 0.2f;	// a right press held still this long on an item opens the menu (the host's kHoldMenu); 0: never

		explicit ItemWindows(ItemWindowsActions actions);
		~ItemWindows();
		void update(const ItemWindowsState& s);	// every frame, before ui::frame
		// For scripted tests.
		Gear* character(uint64_t owner) const;
		ContainerWindow* container(uint64_t owner) const;
		CaptionWindow* window(uint64_t owner) const;	// the character window, else the container window
		GridRef ref(int tag) const;	// an ItemGrid::tag; owner 0: not one of these windows' grids
		uint64_t docked() const { return docked_; }	// the container docked beside the first character window; 0: none
		RightVerb badge() const { return badge_; }	// the badge drawn by the pointer this frame (kNone: none)
		uint32_t badgeMods() const { return badgeMods_; }
		// The stack (kenshi-k16.20.13.1.4): every window on its own root layer, bottom to top. A new window opens on top, a left press anywhere
		// on one raises it, and Escape closes the topmost unpinned one (none: Escape passes to the host).
		const std::vector<uint64_t>& stack() const { return order_; }
		uint64_t top() const;	// the topmost shown window's owner; 0: none
		void raise(uint64_t owner);
		ItemMenu* menu() const;	// the item menu while open, else null
		void openMenu(GridRef at, const GridItem& item, bool held);	// the host's rows at the pointer; no rows: nothing

	private:
		struct Char
		{
			uint64_t owner;
			int base;
			std::unique_ptr<Gear> gear;
		};
		struct Cont
		{
			uint64_t owner;
			int base;
			int cascade;	// steps of kCascade from the docked spot (0: docked)
			bool placed = false;	// a cascaded one is placed once, then stays (its own moves aside)
			Widget* layer = nullptr;	// its own root child (a Gear has its own), so the stack interleaves both kinds
			std::unique_ptr<ContainerWindow> win;
		};
		class Badge;
		ItemWindowsActions act_;
		Badge* badgeW_ = nullptr;
		RightVerb badge_ = RightVerb::kNone;
		uint32_t badgeMods_ = 0;
		ItemMenu* menu_ = nullptr;
		std::vector<uint64_t> order_;	// the stack, bottom to top
		std::vector<Char> chars_;
		std::vector<Cont> conts_;
		uint64_t docked_ = 0;
		uint64_t target_ = 0;
		int nextBase_ = kTagStep;
		std::unique_ptr<Gear> makeGear(uint64_t owner, int base);
		std::unique_ptr<ContainerWindow> makeContainer(uint64_t owner, int base, Widget& layer);
		void markButton(Button& b, uint64_t owner);
		void stackKeys(CaptionWindow& w);	// its Escape is the stack's
		uint64_t escapee() const;	// the topmost shown unpinned window's owner; 0: none
		uint64_t ownerOf(const Widget* w) const;	// the window w is in (any of a Gear's); 0: none of these
		Widget* layer(uint64_t owner) const;
		void dropCont(Cont& c);
		void rightClick(GridRef at, const GridItem& item);	// a quick right click: the menu when the verb says so, else moveAll / quick
		void hover();	// the badge: the verb of the item under the pointer
		void place();
	};
}
