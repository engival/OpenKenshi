#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

struct sapp_event;

// Item grids (docs/UI.md "Item grids"): the original's inventory cells. A grid is cols x rows cells; each item covers its footprint
// ("inventory footprint width / height") from a top-left cell and is drawn as its icon. An equipment slot is a grid in single mode: one item,
// drawn over the whole area whatever its footprint. Items are dragged between grids with the left button; the destination reports the drop
// and the host moves the item (or not): the grids only show the state they are given. A grid taller than viewRows shows that many rows and
// scrolls: the wheel, a scrollbar on its right (arrows, a draggable thumb, page clicks on the track), and a dragged item held near its top or
// bottom edge.
namespace ui
{
	// An item's tooltip as host data (contents after the original's ToolTipInventory, kenshi-inspect findings/inv_click10_static.md 5; the line sets
	// per item kind are the host's). Drawn as the tooltips' rich text: the title, the tags, label / value rows, the description, the hint.
	struct ItemTipTag
	{
		std::string text;	// "[Food]", "Unique", "Research Artifact", "[Masterwork]"
		std::string colour;	// a palette name ("TipGood", "ItemGrade4"); empty: TipText
		std::string right = {};	// on the same line, right-aligned, same colour: "[Medium Armour Class]" ... "[Metal Plate]"
		bool operator==(const ItemTipTag&) const = default;
	};

	struct ItemTipLine
	{
		std::string label, value;	// the host's text, dashes included ("-Weight", "2 kg"); an empty value: a line of its own, wrapped
		bool muted = false;			// TipDim ("-Charges", "-Quality")
		std::string colour = {};	// the value's palette name ("TipGood" for +25%); empty: the line's
		bool operator==(const ItemTipLine&) const = default;
	};

	struct ItemTip
	{
		std::string title;	// the item name; empty: no ItemTip (GridItem::tooltip, else the name)
		uint32_t count = 1;	// " (n)" after the title above 1
		std::vector<ItemTipTag> tags;
		std::vector<ItemTipLine> lines;
		std::string description = {};	// wrapped
		std::string hint = {};		// after the footer, in TipGood: "Right-click to learn tech"
		std::vector<ItemTipTag> footer = {};	// after the description and a blank line, before the hint: "[Model # Edge Type 1]", "[Manufactured by Edgewalkers]", the company text
		bool operator==(const ItemTip&) const = default;
	};

	std::string itemTipText(const ItemTip& t);	// the rich text it draws as

	struct GridItem
	{
		uint32_t id = 0;		// opaque: handed back in callbacks
		int x = 0, y = 0;		// top-left cell (ignored in a single grid)
		int w = 1, h = 1;		// footprint, cells
		std::string icon;		// icons:: path; empty or unreadable: the name is written in the cells
		std::string name;		// the tooltip (when tooltip is empty) and the stand-in for a missing icon
		std::string tooltip;	// rich text (layoutRich); ignored when tip has a title
		ItemTip tip;
		uint32_t count = 1;		// written in the corner when above 1
		float bar = -1;			// 0..1: charges / durability left, the white strip along the bottom (first aid kits); below 0: none (most items)
		Color back = { 0, 0, 0, 0 };	// a tint behind the icon (alpha 0: none)
		int kind = 0;			// the host's slot class, matched against ItemGrid::accepts
		std::string soundSwitch;	// the Inventory_Items switch value of its move sounds (the item's inventory sound; kenshi-inspect rwh65/switches.tsv); empty: none
		bool flipH = false;		// mirrored (on top of the grid's own mirror)
		bool pending = false;	// an order for it is in flight or queued (the host's): drawn dimmed, no press, drag, lift or click, and nothing drops onto it (kenshi-k16.20.14.1)
		bool operator==(const GridItem&) const = default;
	};

	class ItemGrid : public Widget
	{
	public:
		enum class Fit { kNo, kFree, kSwap };	// a drop there: refused, into free cells, onto exactly one other item (swap / stack)

		ItemGrid(int cols = 1, int rows = 1, float cell = 25);
		~ItemGrid() override;
		int cols, rows;
		float cell;				// units per cell (the original's 25)
		bool single = false;	// an equipment slot: one item, drawn at its footprint from the top-left (scaled down if larger), any footprint fits
		bool mirror = false;	// every icon mirrored (the left limbs, when the host's option asks)
		bool blocked = false;	// nothing may go in: every cell drawn blocked (a limb still attached, no boots for this race)
		bool dropIn = true;		// false: nothing may be dropped in, drawn as usual (a building's output slot); its items still drag out
		bool clampDrop = false;	// a drop's top-left is clamped into the grid (an item over it lands where it fits nearest; the footprint-sized building slots)
		std::vector<int> accepts;	// item kinds that may be dropped here; empty: any
		std::vector<GridItem> items;
		int tag = 0;			// the host's name for this grid
		int selected = -1;		// index into items, drawn outlined
		int viewRows = 0;		// rows shown; 0 or rows or fewer: all, no scrollbar
		float scroll = 0;		// units scrolled down (clamped at layout)
		std::function<void(int index, Btn b)> onItemClick;	// left or right click on an item (no drag)
		std::function<void(int index)> onItemActivate;		// double-click
		// The right button held still on an item for holdTime seconds (0: never; the host's menu hold): called while it is still down, and that
		// press then clicks nothing (kenshi-k16.20.13.1.2). The item is found by its place, as a click's is.
		std::function<void(int index)> onItemHold;
		float holdTime = 0;
		static inline bool placeOne = false;	// a lifted stack is placed one unit per click, the rest stays lifted (ui.place_one; the original's setting)
		// Dropped here from `from` (this grid too) with its top-left at cell (x, y) (single: 0, 0); only called when fits() allows it.
		std::function<void(ItemGrid& from, const GridItem& item, int x, int y)> onDrop;
		// Let go here from `from` where fits() refuses it (the drag snaps back): the host says why.
		std::function<void(ItemGrid& from, const GridItem& item)> onRefused;
		// Dragged out of this grid and let go over no grid (the world: drop on the ground), pixels.
		std::function<void(const GridItem& item, float x, float y)> onDropOutside;

		Size preferred() const override;
		void place(const Rect& px, float scale) override;
		std::string tip() const override;
		bool scrolls() const { return viewRows > 0 && rows > viewRows && !single; }
		Rect view() const;	// the cells' visible area, pixels (the grid less its scrollbar)
		Rect bar() const;	// the scrollbar, pixels
		Rect thumb() const;
		int itemAt(float px, float py) const;	// index under the point (pixels), -1 none
		bool cellAt(float px, float py, int& cx, int& cy) const;	// false outside the cells
		// The item (from `from`) at cell (x, y); while it is dragged out of this grid, its own cells count as free.
		Fit fits(const GridItem& item, int x, int y, const ItemGrid* from) const;
		uint32_t shownCount(int index) const;	// the count drawn on item index: the rest while part of its stack is carried, else its count
		float itemAlpha(int index) const;	// how opaque item index draws: a dragged-from one 0.3, a pending one kPendingAlpha, else 1
		static constexpr float kPendingAlpha = 0.4f;
		bool takes(const GridItem& item) const { return accepts.empty() || std::find(accepts.begin(), accepts.end(), item.kind) != accepts.end(); }	// its kind is accepted
		static const GridItem* dragged();	// the item being dragged, null when none

		bool onPress(Btn b, float x, float y) override;
		void onDrag(float x, float y) override;
		void onRelease(Btn b, float x, float y) override;
		bool onScroll(float dy) override;
		void onClick(Btn b) override;
		void onDoubleClick(Btn b) override;

	protected:
		void paint() const override;

	private:
		int press_ = -1;	// the item pressed
		float pressX_ = 0, pressY_ = 0;
		GridItem pressed_;	// the item pressed, as it was: found again by its place before any callback (kenshi-k16.20.14.3)
		double rightAt_ = -1;	// a right press on an item: when; < 0 none (or the hold has fired)
		float rightX_ = 0, rightY_ = 0;
		bool held_ = false;	// the hold fired for this press: its release clicks nothing
		bool pressAll_ = false;	// Shift or Ctrl held at the press: a lift or drag carries the whole stack
		float thumbGrab_ = -1;	// dragging the scrollbar's thumb: the grab point from its top, pixels; < 0 none
		double edgeT_ = -1;		// the last auto-scroll step's time (a dragged item near an edge); < 0 none
		float maxScroll() const;	// units
		float top() const;		// the first row's top, pixels
		void paintBar() const;
		void edgeScroll();	// called at layout while an item is dragged over this grid
		void holdCheck();	// called at layout: a right press held long enough fires onItemHold
		Rect cellRect(int x, int y, int w, int h) const;	// pixels
		Rect itemRect(const GridItem& it) const;
		void drawItem(const GridItem& it, const Rect& r, float alpha, bool flip, bool countOne = false) const;	// countOne: write a count of 1 too (part of a stack carried)
		void dropTarget(float x, float y, ItemGrid*& grid, int& cx, int& cy) const;
		int refind(const GridItem& was) const;	// the index of the item at was's place (x, y, w, h, name, icon; a single slot: no x, y); -1 gone
		int pressedNow();	// press_ checked against pressed_: the item's current index, or -1 (press_ too) when it has gone
		bool track();	// the carried item followed in this (the source) grid: index and id updated; gone: the drag ends, false
		void lift(int index, float x, float y, bool all);	// a click took the item up (grabbed at x, y): all, the whole stack, else one unit
		static bool liftEvent(const sapp_event& e);
		static void showGhost();	// the dragged or lifted item under the pointer, at the source's cell size	// the interceptor while an item is lifted	// the grid under the pointer and the item's top-left there
	};

	// The first free top-left for a w x h footprint, row by row (the original's auto placement); false when it fits nowhere.
	bool findSpace(int cols, int rows, const std::vector<GridItem>& items, int w, int h, int& x, int& y);
	// ARRANGE: places every item again, biggest first, each at the first free place; false (that item's x = y = -1) when one fits nowhere.
	bool arrange(int cols, int rows, std::vector<GridItem>& items);
}
