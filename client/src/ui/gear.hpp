#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/itemgrid.hpp"

// The grid inventory screen (docs/UI.md "Item grids"), content after the original's (Kenshi_InventoryCharacterWindow / InventoryGenericWindow /
// LimbReplacementWindow layouts), layout ours. The character window: OPEN BAG / LIMBS, the worn backpack, WEAPON I / WEAPON II / BELT,
// SHIRT / PANTS / BOOTS / HEAD / ARMOR slots, a stats panel and the main inventory with ARRANGE. Beside it, opened from it: the backpack's contents
// (left), ROBOTICS (right: arm and leg slots around the body figure), and a partner's container (ui::ItemWindows). Every grid's size comes from the state (backpacks,
// races and containers differ). Like the Inventory it knows nothing about SIMLINK: the host fills a GearState and turns GearActions into orders.
// A looted body (SIMLINK 13 loot open) is a second Gear, docked right of ours with its grids numbered from kGearOther: the original shows it as a
// character window beside the looter's, worn slots included (UNSURE until kenshi-inspect's capture: its exact layout).
namespace ui
{
	// The screen's grids: GearState::grids order, and the grid numbers handed back in actions.
	enum GearGrid
	{
		kGearBackpack,	// the worn backpack (slot)
		kGearWeapon1, kGearWeapon2, kGearBelt, kGearShirt, kGearHead, kGearPants, kGearArmour, kGearBoots,	// equipment slots
		kGearMain,		// the character's own inventory
		kGearBag,		// the backpack's contents (OPEN BAG)
		kGearArmRight, kGearArmLeft, kGearLegRight, kGearLegLeft,	// robotics (LIMBS); right is drawn on the screen's left, facing the viewer
		kGearContainer,	// an open container beside the character
		kGearGrids
	};
	constexpr int kGearOther = 200;	// a second Gear's (a looted body's) grid numbers: kGearOther + kGear*, so a drag between the two names its source

	int gearTop();	// ui.gear_top: the item windows' top edge, units at 1080 lines; -1 centres them

	enum class GearDock { kCentre, kLeft, kRight };	// alone; the looter's beside a body's (its side windows left); the body's (its side windows right)

	struct GearGridState
	{
		int cols = 0, rows = 0;	// 0: the grid is absent (hidden with its caption; no bag -> OPEN BAG disabled)
		bool single = false;	// an equipment slot (one item over the whole area); the equipment and limb grids usually are
		bool blocked = false;	// nothing may go in (a limb still attached)
		std::vector<int> accepts;	// GridItem kinds it takes; empty: any
		std::vector<GridItem> items;
		bool operator==(const GearGridState&) const = default;
	};

	struct GearInfo	// a line of the stats panel: "Total Weight" "19/101"
	{
		std::string label, value;
		std::string colour;	// the value's named colour ("Bad"); empty: the text colour
		std::string tip;	// hover text (rich), in the HUD's fixed box under ui.hud_tips=box (the original's: Disguise, Slave of, Looks like slave)
		bool dim = false;	// the whole row greyed (a free person's slave rows)
		bool operator==(const GearInfo&) const = default;
	};

	// Which robotics slots draw their icons mirrored. Limb items are one record for both sides, so the original draws a left leg as a right one
	// (mods shipped mirrored copies of every limb); we mirror at draw time instead.
	enum class LimbMirror { kNone, kLeft, kRight };

	// Trading (kenshi-inspect findings/ui_captures/trade_window.md): the container window is the trader's. Its caption is the trader's name in
	// capitals, a "Money: c.N" box over the grid shows the TRADER's cats (ours are only on the HUD). Buying and selling are the ordinary moves:
	// a drag, or the host's right-click (1 unit; Shift: the stack); no confirm, no count prompt. Prices are ItemTip lines; the fence prompt is
	// trade::askFence; refusals ("You can't afford that.", ...) are the host's toasts, and a refused drag snaps back as any unanswered move.
	struct GearTrade
	{
		bool active = false;
		int64_t cats = 0;	// the trader's
		bool operator==(const GearTrade&) const = default;
	};

	// The container window's kind, after the original's three layouts: Kenshi_InventoryGenericWindow (a closable window), InventoryGenericFixedWindow
	// (Kenshi_WindowC: no close button; a building's own storage (?)) and InventoryResearchWindow (closable, a SHOW RESEARCH button over the grid).
	enum class ContainerKind { kGeneric, kFixed, kResearch };

	// One container window's state (a box, a body's stock, a trader's: ui::ContainerWindow; several at once through ui::ItemWindows).
	struct ContainerState
	{
		uint64_t owner = 0;
		std::string name;	// empty: "CONTAINER"
		bool foreign = false;	// taking from it is theft (title in red; not while trading)
		ContainerKind kind = ContainerKind::kGeneric;
		std::string type;	// TypeText over the grid (Kenshi_GenericTextBoxFlat, centred); empty: hidden
		std::string capacity;	// CapacityText under it: "Capacity: 50 (Full)"; empty: hidden
		GearTrade trade;	// active: a trader's stock
		GearGridState grid;
		bool pinned = false;	// the pin drawn checked (set when it changes); the host keeps a pinned window open
		bool operator==(const ContainerState&) const = default;
	};

	// A container's window (Kenshi_InventoryGenericWindow and its fixed / research kinds): the grid with its caption, the trader's money, TypeText
	// and CapacityText, SHOW RESEARCH and ARRANGE. Its owner wires the grid (tag, onDrop, ...) and the callbacks; shown while the owner says.
	class ContainerWindow
	{
	public:
		ContainerWindow(Widget& screen, std::function<void()> onClose);	// added to screen, hidden
		~ContainerWindow();
		void update(const ContainerState& s);	// cheap when nothing changed
		const ContainerState& state() const { return last_; }
		bool open() const { return last_.grid.cols > 0 && last_.grid.rows > 0; }	// has a grid to show
		CaptionWindow& window() const { return *win_; }
		ItemGrid& grid() const { return *grid_; }
		std::function<void()> onArrange, onResearch;
		Widget* arrangeButton() const { return arrange_; }
		Widget* traderMoney() const { return money_; }
		Widget* researchButton() const { return research_; }
		Widget* typeText() const { return type_; }
		Widget* capacityText() const { return cap_; }

	private:
		Widget* screen_;
		CaptionWindow* win_ = nullptr;
		Label* title_ = nullptr;
		ItemGrid* grid_ = nullptr;
		Label* money_ = nullptr;
		Button* research_ = nullptr;
		Button* arrange_ = nullptr;
		Label *type_ = nullptr, *cap_ = nullptr;
		ContainerState last_;
	};

	struct GearState
	{
		bool valid = false;	// false: nothing to show yet
		uint64_t owner = 0;
		std::string name;			// the window title
		bool nameForeign = false;	// the title in red: a body of another faction (taking from it is theft)
		std::string bagName;		// the backpack window's title; empty: "BACKPACK"
		GearGridState grids[kGearGrids];	// grids[kGearContainer] unused: a container is a ContainerState (ItemWindows, kenshi-k16.20.13.1.3)
		std::vector<GearInfo> info;
		LimbMirror mirror = LimbMirror::kLeft;
		bool operator==(const GearState&) const = default;
	};

	// Grids are kGear* numbers. The screen moves nothing itself: the host answers with the next state.
	struct GearActions
	{
		// Dragged from one grid to another (or within one) with its top-left at cell (x, y); the destination said it fits (free or onto one item).
		std::function<void(uint64_t owner, int fromGrid, const GridItem& item, int toGrid, int x, int y)> move;
		std::function<void(uint64_t owner, int grid, const GridItem& item)> activate;	// double-click (eat, read, equip)
		std::function<void(uint64_t owner, int grid, const GridItem& item, float x, float y)> context;	// right-click, pixels (the host's menu)
		std::function<void(uint64_t owner, int grid, const GridItem& item)> moveAll;	// Ctrl + right-click (ours, kenshi-k16.20.12.1): every item like this one, moved as a right-click would
		std::function<void(uint64_t owner, int grid, const GridItem& item)> dropOutside;	// dragged out over the world: drop it
		std::function<void(uint64_t owner, int grid, const GridItem& item)> hold;	// the right button held still on it (ItemGrid::holdTime; ItemWindows' menu)
		std::function<void(uint64_t owner, int grid)> arrange;	// ARRANGE (ui::arrange computes a packing the host may use)
		std::function<void()> close;	// the character window's close button; unset: it hides itself (a body's: the host ends the loot)
	};

	class Gear
	{
	public:
		explicit Gear(GearActions actions, int tagBase = 0);	// tagBase: 0, or kGearOther for a body's
		~Gear();
		void update(const GearState& s);	// every frame, before ui::frame (cheap when nothing changed)
		bool shown() const;
		void show(bool on);	// the character window (and what it opened); hiding it unpins it
		bool pinned() const;	// the character window's pin: Escape spares it, and the host keeps its owner while the selection changes
		bool bagShown() const { return bag_; }
		void showBag(bool on);
		bool limbsShown() const { return limbs_; }
		void showLimbs(bool on);
		void dock(GearDock d);
		void offset(float x, float y);	// every window moved by (x, y) units from where dock() puts them (ui::ItemWindows' third and later)
		// For scripted tests.
		ItemGrid* grid(int g) const { return g >= 0 && g < kGearGrids ? grids_[g] : nullptr; }	// a kGear* number, without tagBase
		Widget* bagButton() const { return bagButton_; }
		Widget* limbsButton() const { return limbsButton_; }
		Widget* pinButton() const;
		Widget* arrangeButton(int g) const;	// kGearMain, kGearBag, kGearContainer (the partner's; null without one)
		Widget* window(int g) const;		// the window holding grid g (kGearContainer: the partner's, else null)
		Widget* infoValue(size_t i) const { return i < info_.size() ? info_[i] : nullptr; }	// a Label
		// ui::ItemWindows: the character window, and the container window docked beside it (null: none; Gear has no container of its own).
		CaptionWindow& characterWindow() const { return *char_; }
		Widget& layer() const { return *screen_; }	// the root child holding its windows (ItemWindows stacks these)
		void partner(ContainerWindow* c);
		void place();	// the side windows beside the character window (update() does it; after a partner's change)

	private:
		Widget* screen_ = nullptr;
		CaptionWindow* char_ = nullptr;
		CaptionWindow* bagWin_ = nullptr;
		CaptionWindow* limbWin_ = nullptr;
		ContainerWindow* partner_ = nullptr;
		Label *title_ = nullptr, *bagTitle_ = nullptr;
		Label* empty_ = nullptr;
		Widget* body_ = nullptr;
		ItemGrid* grids_[kGearGrids] = {};
		Widget* groups_[kGearGrids] = {};	// a grid with its caption (side windows)
		Label* caps_[kGearGrids] = {};		// the caption of a grid placed on the character window's lattice
		Button *bagButton_ = nullptr, *limbsButton_ = nullptr;
		Button *arrangeMain_ = nullptr, *arrangeBag_ = nullptr;
		Panel* infoPanel_ = nullptr;
		Box* infoRows_ = nullptr;
		std::vector<Label*> info_;
		GearActions act_;
		int base_ = 0;
		GearDock dock_ = GearDock::kCentre;
		float offX_ = 0, offY_ = 0;
		GearState last_;
		bool shown_ = false, bag_ = false, limbs_ = false;	// shown_: show(); the character window is drawn while it is
		int selGrid_ = -1;
		uint32_t selId_ = 0;	// the selected item: its grid and id (kept across states while it is there)
		ItemGrid& addGrid(Box& parent, const char* caption, int g);
		ItemGrid& addSlot(Widget& canvas, const char* caption, int g);	// placed by layoutChar
		void wire(ItemGrid& grid, int g);
		void layoutChar();	// the character window's grids on the cell lattice, from their sizes
		CaptionWindow& addWindow(Label*& title, std::function<void()> close);
		void select(int g, int i);
		void rebuildInfo();
	};
}
