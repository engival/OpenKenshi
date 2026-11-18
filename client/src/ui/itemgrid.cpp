#include "ui/itemgrid.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "ui/icons.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// The one drag in progress. The ghost is recorded by ui::record (setDragImage); the grids under the pointer read the target to show the drop.
		struct Drag
		{
			ItemGrid* from = nullptr;	// null: no drag
			GridItem item;
			int index = -1;			// in from->items when it started
			float ox = 0, oy = 0;	// the grab point from the footprint's top-left, pixels at the footprint's own size
			bool flip = false;
			ItemGrid* target = nullptr;
			int tx = 0, ty = 0;
			ItemGrid::Fit fit = ItemGrid::Fit::kNo;
			uint32_t stack = 1;		// the source item's count; item.count is what is carried (kenshi-k16.20.12.1)
			bool part() const { return item.count < stack; }	// part of a stack carried: the rest stays and keeps its cells
			bool lifted = false;	// taken up by a click, not held down: the next click places it
			double at = 0;			// when it was lifted
		};
		Drag drag_;
		bool eatUp_ = false;	// the release of the click that placed or put back a lifted item is ours too

		// The scrollbar, units: after Kenshi_ScrollBarV (17 wide, 12-high arrows, the thumb at least 10).
		const float kBarW = 17, kArrowH = 12, kThumbMinH = 10;
		const float kEdge = 25, kEdgeRate = 250;	// a dragged item within kEdge of the top or bottom scrolls the grid kEdgeRate units a second

		// The footprint in pixels (both grids draw cells at their own size; the ghost uses the source's).
		Size footprint(const GridItem& it, const ItemGrid& g, float scale) { return { it.w * g.cell * scale, it.h * g.cell * scale }; }

		bool overlaps(const GridItem& a, int x, int y, int w, int h) { return a.x < x + w && x < a.x + a.w && a.y < y + h && y < a.y + a.h; }

		void endDrag()
		{
			const bool lifted = drag_.lifted;
			drag_ = {};
			setDragImage({});
			if (lifted && !eatUp_) { setInterceptor({}); }
		}

	}

	ItemGrid::ItemGrid(int cols, int rows, float cell) : cols(cols), rows(rows), cell(cell)
	{
		solid = true;
	}

	ItemGrid::~ItemGrid()
	{
		if (drag_.from == this)
		{
			eatUp_ = false;
			endDrag();
		}
		if (drag_.target == this) { drag_.target = nullptr; }
	}

	Size ItemGrid::preferred() const
	{
		if (!scrolls()) { return { cols * cell, rows * cell }; }
		return { cols * cell + kBarW, viewRows * cell };
	}

	float ItemGrid::maxScroll() const { return scrolls() ? (rows - viewRows) * cell : 0; }
	float ItemGrid::top() const { return screen_.y - std::round(scroll * scale_); }

	Rect ItemGrid::view() const
	{
		if (!scrolls()) { return screen_; }
		return { screen_.x, screen_.y, std::max(0.0f, screen_.w - std::round(kBarW * scale_)), screen_.h };
	}

	Rect ItemGrid::bar() const
	{
		const Rect v = view();
		return { v.right(), screen_.y, screen_.right() - v.right(), screen_.h };
	}

	Rect ItemGrid::thumb() const
	{
		const Rect b = bar();
		const float a = std::round(kArrowH * scale_), track = std::max(0.0f, b.h - 2 * a), total = rows * cell;
		const float h = std::min(track, std::max(std::round(kThumbMinH * scale_), std::round(track * viewRows * cell / total)));
		const float m = maxScroll();
		return { b.x, std::round(b.y + a + (m > 0 ? (track - h) * scroll / m : 0)), b.w, h };
	}

	// The host's ids may be list indexes, renumbered when an earlier entry leaves; an item's place does not move then (kenshi-k16.20.14.3).
	int ItemGrid::refind(const GridItem& was) const
	{
		for (size_t i = 0; i < items.size(); i++)
		{
			const GridItem& it = items[i];
			if ((single || (it.x == was.x && it.y == was.y)) && it.w == was.w && it.h == was.h && it.name == was.name && it.icon == was.icon) { return (int)i; }
		}
		return -1;
	}

	int ItemGrid::pressedNow()
	{
		if (press_ >= 0) { press_ = refind(pressed_); }
		return press_;
	}

	bool ItemGrid::track()
	{
		if (drag_.from != this) { return true; }
		const int i = refind(drag_.item);
		if (i < 0)	// sold, dropped or used meanwhile: nothing to place
		{
			endDrag();
			return false;
		}
		drag_.index = i;
		drag_.item.id = items[i].id;
		drag_.stack = std::max<uint32_t>(1, items[i].count);
		drag_.item.count = std::min(drag_.item.count, drag_.stack);
		return true;
	}

	void ItemGrid::place(const Rect& px, float s)
	{
		Widget::place(px, s);
		track();
		scroll = std::clamp(scroll, 0.0f, maxScroll());
		edgeScroll();
		holdCheck();
	}

	void ItemGrid::holdCheck()
	{
		if (rightAt_ < 0) { return; }
		if (!pressed(this) || !onItemHold || holdTime <= 0)
		{
			if (!pressed(this)) { rightAt_ = -1; }
			return;
		}
		float mx, my;
		mouse(mx, my);
		const float slop = 4 * scale_;
		if (std::fabs(mx - rightX_) > slop || std::fabs(my - rightY_) > slop)	// moved: a plain right click when it comes up
		{
			rightAt_ = -1;
			return;
		}
		if (now() - rightAt_ < holdTime) { return; }
		rightAt_ = -1;
		const int i = pressedNow();
		if (i < 0) { return; }
		held_ = true;
		auto f = onItemHold;
		f(i);
	}

	void ItemGrid::edgeScroll()
	{
		if (!scrolls() || !drag_.from || drag_.target != this)
		{
			edgeT_ = -1;
			return;
		}
		float mx, my;
		mouse(mx, my);
		const Rect v = view();
		const float edge = kEdge * scale_;
		const int dir = my < v.y + edge ? -1 : my > v.bottom() - edge ? 1 : 0;
		const double t = now();
		if (dir != 0 && edgeT_ >= 0)
		{
			const float before = scroll;
			scroll = std::clamp(scroll + dir * kEdgeRate * (float)std::min(0.1, t - edgeT_), 0.0f, maxScroll());
			if (scroll != before)	// the cell under the pointer moved: aim the drop again
			{
				drag_.from->dropTarget(mx, my, drag_.target, drag_.tx, drag_.ty);
				drag_.fit = drag_.target ? drag_.target->fits(drag_.item, drag_.tx, drag_.ty, drag_.from) : Fit::kNo;
			}
		}
		edgeT_ = dir != 0 ? t : -1;
	}

	Rect ItemGrid::cellRect(int x, int y, int w, int h) const
	{
		const float c = cell * scale_;
		const float t = top();
		return { std::round(screen_.x + x * c), std::round(t + y * c), std::round(screen_.x + (x + w) * c) - std::round(screen_.x + x * c),
			std::round(t + (y + h) * c) - std::round(t + y * c) };
	}

	Rect ItemGrid::itemRect(const GridItem& it) const
	{
		if (!single) { return cellRect(it.x, it.y, it.w, it.h); }
		// A slot shows the item at its own footprint from the top-left, as the original does (a 3 x 3 hat leaves a column of a 4 x 3 slot
		// empty); one larger than the slot is scaled down to fit.
		const float c = cell * scale_, k = std::min({ 1.0f, (float)cols / std::max(1, it.w), (float)rows / std::max(1, it.h) });
		return { std::round(screen_.x), std::round(screen_.y), std::round(it.w * k * c), std::round(it.h * k * c) };
	}

	int ItemGrid::itemAt(float px, float py) const
	{
		if (!view().contains(px, py)) { return -1; }
		for (size_t i = items.size(); i-- > 0;)
		{
			if (itemRect(items[i]).contains(px, py)) { return (int)i; }
		}
		return -1;
	}

	bool ItemGrid::cellAt(float px, float py, int& cx, int& cy) const
	{
		const float c = cell * scale_;
		if (c <= 0 || !view().contains(px, py)) { return false; }
		cx = std::clamp((int)((px - screen_.x) / c), 0, cols - 1);
		cy = std::clamp((int)((py - top()) / c), 0, rows - 1);
		return true;
	}

	ItemGrid::Fit ItemGrid::fits(const GridItem& item, int x, int y, const ItemGrid* from) const
	{
		if (blocked || !dropIn || cols <= 0 || rows <= 0) { return Fit::kNo; }
		if (!takes(item)) { return Fit::kNo; }
		const int self = from == this && drag_.from == this && !drag_.part() ? drag_.index : -1;	// the dragged item does not block its own way (a part's rest does)
		if (single)
		{
			const size_t others = items.size() - (self >= 0 && self < (int)items.size() ? 1 : 0);
			if (std::any_of(items.begin(), items.end(), [](const GridItem& o) { return o.pending; })) { return Fit::kNo; }
			return others == 0 ? Fit::kFree : others == 1 ? Fit::kSwap : Fit::kNo;
		}
		if (x < 0 || y < 0 || x + item.w > cols || y + item.h > rows) { return Fit::kNo; }
		int hits = 0;
		for (size_t i = 0; i < items.size(); i++)
		{
			if ((int)i == self || !overlaps(items[i], x, y, item.w, item.h)) { continue; }
			if (items[i].pending) { return Fit::kNo; }	// mid-order: nothing swaps or stacks onto it
			hits++;
		}
		return hits == 0 ? Fit::kFree : hits == 1 ? Fit::kSwap : Fit::kNo;
	}

	// The drag state for item i grabbed at (x, y): the copy carries one unit, or the whole stack when all
	static Drag grab(ItemGrid* g, const GridItem& it, int i, float x, float y, const Rect& r, Size f, bool mirror, bool all)
	{
		Drag d = { g, it, i, (x - r.x) / std::max(1.0f, r.w) * f.w, (y - r.y) / std::max(1.0f, r.h) * f.h, it.flipH != mirror };
		d.stack = std::max<uint32_t>(1, it.count);
		d.item.count = all ? d.stack : 1;
		return d;
	}

	uint32_t ItemGrid::shownCount(int i) const
	{
		if (i < 0 || i >= (int)items.size()) { return 0; }
		return drag_.from == this && drag_.index == i && drag_.part() ? drag_.stack - drag_.item.count : items[i].count;
	}

	float ItemGrid::itemAlpha(int i) const
	{
		if (drag_.from == this && drag_.index == i && !drag_.part()) { return 0.3f; }
		return i >= 0 && i < (int)items.size() && items[i].pending ? kPendingAlpha : 1.0f;
	}

	const GridItem* ItemGrid::dragged() { return drag_.from ? &drag_.item : nullptr; }

	std::string ItemGrid::tip() const
	{
		if (drag_.from) { return {}; }
		float mx, my;
		mouse(mx, my);
		const int i = itemAt(mx, my);
		if (i < 0) { return tooltip; }
		const GridItem& it = items[i];
		return !it.tip.title.empty() ? itemTipText(it.tip) : it.tooltip.empty() ? it.name : it.tooltip;
	}

	// A tag's line; the right text is the line's value, so it right-aligns in the line's colour
	static std::string tagLine(const ItemTipTag& g)
	{
		return "\n{" + (g.colour.empty() ? std::string("TipText") : g.colour) + "}" + g.text + (g.right.empty() ? std::string() : "\t" + g.right);
	}

	std::string itemTipText(const ItemTip& t)
	{
		std::string s = "{TipTitle}" + t.title + (t.count > 1 ? " (" + std::to_string(t.count) + ")" : std::string());
		for (const ItemTipTag& g : t.tags) { s += tagLine(g); }
		for (const ItemTipLine& l : t.lines)
		{
			s += "\n" + std::string(l.muted ? "{TipDim}" : "") + l.label;
			if (!l.value.empty()) { s += "\t" + (l.colour.empty() ? std::string() : "{" + l.colour + "}") + l.value; }
		}
		if (!t.description.empty()) { s += "\n" + t.description; }
		if (!t.footer.empty()) { s += "\n"; }
		for (const ItemTipTag& g : t.footer) { s += tagLine(g); }
		if (!t.hint.empty()) { s += "\n{TipGood}" + t.hint; }
		return s;
	}

	bool ItemGrid::onPress(Btn b, float x, float y)
	{
		thumbGrab_ = -1;
		if (scrolls() && bar().contains(x, y))	// the scrollbar: an arrow steps a row, the thumb drags, the track pages
		{
			press_ = -1;
			pressX_ = -1e9f;
			if (b != Btn::kLeft) { return true; }
			const Rect br = bar(), t = thumb();
			const float a = std::round(kArrowH * scale_);
			if (y < br.y + a) { scroll -= cell; }
			else if (y >= br.bottom() - a) { scroll += cell; }
			else if (y < t.y) { scroll -= (viewRows - 1) * cell; }
			else if (y >= t.bottom()) { scroll += (viewRows - 1) * cell; }
			else { thumbGrab_ = y - t.y; }
			scroll = std::clamp(scroll, 0.0f, maxScroll());
			return true;
		}
		press_ = b == Btn::kMiddle ? -1 : itemAt(x, y);
		if (press_ >= 0 && items[press_].pending) { press_ = -1; }	// mid-order: the press is eaten, nothing lifts or clicks
		if (press_ >= 0) { pressed_ = items[press_]; }
		pressX_ = x;
		pressY_ = y;
		pressAll_ = (mods() & (SAPP_MODIFIER_SHIFT | SAPP_MODIFIER_CTRL)) != 0;
		held_ = false;
		rightAt_ = b == Btn::kRight && press_ >= 0 ? now() : -1;
		rightX_ = x;
		rightY_ = y;
		if (b != Btn::kLeft) { pressX_ = -1e9f; }	// only the left button drags
		return true;
	}

	void ItemGrid::onDrag(float x, float y)
	{
		if (thumbGrab_ >= 0)
		{
			const Rect br = bar(), t = thumb();
			const float a = std::round(kArrowH * scale_), room = br.h - 2 * a - t.h;
			if (room > 0) { scroll = std::clamp((y - thumbGrab_ - br.y - a) / room * maxScroll(), 0.0f, maxScroll()); }
			return;
		}
		if (pressX_ <= -1e9f)	// a right (or middle) press drags nothing; moved off its item, it clicks nothing either
		{
			if (press_ >= 0 && itemAt(x, y) != press_) { press_ = -1; }
			return;
		}
		if (!drag_.from)
		{
			if (press_ < 0 || std::hypot(x - pressX_, y - pressY_) < 5 * scale_ || pressedNow() < 0 || items[press_].pending) { return; }
			const GridItem& it = items[press_];
			drag_ = grab(this, it, press_, pressX_, pressY_, itemRect(it), footprint(it, *this, scale_), mirror, pressAll_);
			showGhost();
		}
		dropTarget(x, y, drag_.target, drag_.tx, drag_.ty);
		drag_.fit = drag_.target ? drag_.target->fits(drag_.item, drag_.tx, drag_.ty, this) : Fit::kNo;
	}

	void ItemGrid::dropTarget(float x, float y, ItemGrid*& grid, int& cx, int& cy) const
	{
		grid = dynamic_cast<ItemGrid*>(root().hit(x, y));
		cx = cy = 0;
		if (!grid || grid->single) { return; }
		// The cell nearest the footprint's top-left corner, at the target's cell size.
		const float c = grid->cell * scale();
		const float k = grid->cell / cell;
		cx = (int)std::floor((x - drag_.ox * k - grid->screen().x) / c + 0.5f);
		cy = (int)std::floor((y - drag_.oy * k - grid->top()) / c + 0.5f);
		if (grid->clampDrop)
		{
			cx = std::clamp(cx, 0, std::max(0, grid->cols - drag_.item.w));
			cy = std::clamp(cy, 0, std::max(0, grid->rows - drag_.item.h));
		}
	}

	void ItemGrid::onRelease(Btn, float x, float y)
	{
		thumbGrab_ = -1;
		if (drag_.from != this) { return; }
		press_ = -1;	// no click after a drag
		if (!track()) { return; }	// the item left while it was held
		const Drag d = drag_;
		endDrag();
		if (d.target)
		{
			if (d.fit != Fit::kNo && d.target->onDrop) { d.target->onDrop(*this, d.item, d.tx, d.ty); }
			else if (d.fit == Fit::kNo && d.target->onRefused) { d.target->onRefused(*this, d.item); }
		}
		else if (!root().hit(x, y) && onDropOutside) { onDropOutside(d.item, x, y); }
	}

	bool ItemGrid::onScroll(float dy)
	{
		if (!scrolls()) { return false; }
		scroll = std::clamp(scroll - dy * cell, 0.0f, maxScroll());	// a row a notch
		return true;
	}

	void ItemGrid::onClick(Btn b)
	{
		rightAt_ = -1;
		if (b == Btn::kRight && held_) { held_ = false; return; }	// the hold opened the menu: this release is not a click
		if (pressedNow() < 0) { return; }	// the item pressed has gone (or another took its index): the click is eaten
		if (onItemClick) { onItemClick(press_, b); }
		// The original's way (kenshi-inspect inv_click10_static.md 1): a left click lifts one unit, Shift or Ctrl + left the whole stack.
		if (b == Btn::kLeft && !drag_.from && press_ < (int)items.size()) { lift(press_, pressX_, pressY_, pressAll_); }
	}

	void ItemGrid::showGhost()
	{
		setDragImage([](float px, float py)
		{
			if (!drag_.from || !drag_.from->shown()) { return; }
			const Size f = footprint(drag_.item, *drag_.from, scale());
			const Rect r = { std::round(px - drag_.ox), std::round(py - drag_.oy), std::round(f.w), std::round(f.h) };
			drag_.from->drawItem(drag_.item, r, 0.85f, drag_.flip, drag_.stack > 1);
		});
	}

	void ItemGrid::lift(int i, float x, float y, bool all)
	{
		if (items[i].pending) { return; }
		const GridItem& it = items[i];
		drag_ = grab(this, it, i, x, y, itemRect(it), footprint(it, *this, scale_), mirror, all);
		drag_.lifted = true;
		drag_.at = now();
		showGhost();
		dropTarget(x, y, drag_.target, drag_.tx, drag_.ty);
		drag_.fit = drag_.target ? drag_.target->fits(drag_.item, drag_.tx, drag_.ty, this) : Fit::kNo;
		setInterceptor(&ItemGrid::liftEvent);
	}

	// A lifted item follows the pointer; the next left press places it as a drag's release would (a grid: onDrop / onRefused; no UI: the
	// world), on its own place or a second click within the double-click time on it (then onItemActivate) puts it back; a right press or
	// Escape puts it back; a press on other UI keeps it lifted. Every press it takes is consumed with its release.
	bool ItemGrid::liftEvent(const sapp_event& e)
	{
		if (e.type == SAPP_EVENTTYPE_MOUSE_UP && eatUp_)
		{
			eatUp_ = false;
			if (!drag_.lifted) { setInterceptor({}); }
			return true;
		}
		if (!drag_.lifted) { return false; }
		ItemGrid* from = drag_.from;
		if (!from->shown())	// its window closed: back where it was
		{
			endDrag();
			return false;
		}
		const float x = e.mouse_x, y = e.mouse_y;
		switch (e.type)
		{
			case SAPP_EVENTTYPE_MOUSE_MOVE:
				from->dropTarget(x, y, drag_.target, drag_.tx, drag_.ty);
				drag_.fit = drag_.target ? drag_.target->fits(drag_.item, drag_.tx, drag_.ty, from) : Fit::kNo;
				return false;
			case SAPP_EVENTTYPE_KEY_DOWN:
				if (e.key_code != SAPP_KEYCODE_ESCAPE) { return false; }
				endDrag();
				return true;
			case SAPP_EVENTTYPE_MOUSE_DOWN:
			{
				eatUp_ = true;
				if (!from->track()) { return true; }	// the item left while it was lifted: this press places nothing
				if (e.mouse_button != SAPP_MOUSEBUTTON_LEFT)
				{
					endDrag();
					return true;
				}
				from->dropTarget(x, y, drag_.target, drag_.tx, drag_.ty);
				drag_.fit = drag_.target ? drag_.target->fits(drag_.item, drag_.tx, drag_.ty, from) : Fit::kNo;
				const Drag d = drag_;
				if (d.target == from && now() - d.at < 0.35 && from->itemAt(x, y) == d.index)	// a double click: use it
				{
					endDrag();
					if (from->onItemActivate) { from->onItemActivate(d.index); }
					return true;
				}
				if (d.target)
				{
					const bool own = d.target == from && (from->single || (d.tx == d.item.x && d.ty == d.item.y));
					const bool one = placeOne && d.item.count > 1 && !own && d.fit != Fit::kNo;	// ui.place_one: one unit goes, the rest stays lifted
					GridItem it = d.item;
					if (one)
					{
						drag_.item.count--;
						drag_.at = 0;	// the next click is no double click
						it.count = 1;
					}
					else { endDrag(); }
					if (own) { return true; }	// its own place
					if (d.fit != Fit::kNo && d.target->onDrop) { d.target->onDrop(*from, it, d.tx, d.ty); }
					else if (d.fit == Fit::kNo && d.target->onRefused) { d.target->onRefused(*from, d.item); }
					return true;
				}
				if (!root().hit(x, y))
				{
					endDrag();
					if (from->onDropOutside) { from->onDropOutside(d.item, x, y); }
				}
				return true;
			}
			default:
				return false;
		}
	}

	void ItemGrid::onDoubleClick(Btn b)
	{
		if (b == Btn::kLeft && onItemActivate) { if (pressedNow() >= 0) { onItemActivate(press_); } }
		else { onClick(b); }
	}

	void ItemGrid::drawItem(const GridItem& it, const Rect& r, float alpha, bool flip, bool countOne) const
	{
		// After Kenshi_InventoryItem.layout: a dark backdrop over the footprint, the icon stretched over it (the game renders icons at the
		// footprint's shape), the count, and for items with charges or durability a white strip along the bottom.
		const float px = std::max(1.0f, std::round(scale()));
		const Rect in = r.inset(px);
		gfx::fill(in, res::colour("ItemBack", { 0, 0, 0, 0.55f }).alpha(alpha));
		if (it.back.a > 0) { gfx::fill(in, it.back.alpha(alpha)); }
		const icons::Picture& pic = icons::get(it.icon);
		if (pic) { icons::draw(pic, in, Color{}.alpha(alpha), flip, false); }
		const Font* f = fonts::get("Kenshi_StandardFont_Small");
		if (!pic && f)	// no icon: the name, wrapped in the footprint
		{
			const std::string name = it.name.empty() ? "?" : it.name;	// wrap() returns views into it
			const auto lines = f->wrap(name, std::max(1.0f, r.w - 4 * px));
			float y = std::round(r.y + (r.h - lines.size() * f->lineHeight()) / 2);
			for (std::string_view l : lines)
			{
				f->draw(std::round(r.x + (r.w - f->width(l)) / 2), y, l, res::colour("Main").alpha(alpha));
				y += f->lineHeight();
			}
		}
		const float barH = std::max(3.0f, std::round(4 * scale()));
		if (it.bar >= 0)
		{
			const Rect strip = { in.x, in.bottom() - barH, in.w, barH };
			gfx::fill(strip, Color{ 0, 0, 0, alpha });
			const Rect fill = strip.inset(px);
			gfx::fill({ fill.x, fill.y, std::round(fill.w * std::clamp(it.bar, 0.0f, 1.0f)), fill.h }, Color{ 1, 1, 1, alpha });
		}
		if ((it.count > 1 || (countOne && it.count == 1)) && f)
		{
			const std::string n = std::to_string(it.count);
			const float x = std::round(r.right() - 3 * px - f->width(n)), y = std::round(r.bottom() - (it.bar >= 0 ? barH + px : 0) - 2 * px - f->lineHeight());
			f->draw(x + px, y + px, n, Color{ 0, 0, 0, 0.8f * alpha });
			f->draw(x, y, n, res::colour("Main").alpha(alpha));
		}
	}

	void ItemGrid::paint() const
	{
		const bool clip = scrolls();
		const Rect v = view();
		if (clip) { gfx::pushClip(v); }
		const float c = cell * scale_;
		const int y0 = clip && c > 0 ? std::max(0, (int)((v.y - top()) / c)) : 0, y1 = clip && c > 0 ? std::min(rows, (int)((v.bottom() - top()) / c) + 1) : rows;
		for (int y = y0; y < y1; y++)
		{
			for (int x = 0; x < cols; x++) { style().draw(Look::kCell, cellRect(x, y, 1, 1), scale_, blocked ? State::kDisabled : State::kNormal, false); }
		}
		if (drag_.from && drag_.target == this)	// where it would land
		{
			const Rect r = single ? itemRect(drag_.item) : cellRect(drag_.tx, drag_.ty, drag_.item.w, drag_.item.h).intersect(v);
			const Color c = drag_.fit == Fit::kFree ? res::colour("DropOk", { 0.37f, 0.68f, 0.27f, 0.5f })
				: drag_.fit == Fit::kSwap ? res::colour("DropSwap", { 0.85f, 0.64f, 0.25f, 0.5f }) : res::colour("DropNo", { 0.75f, 0.31f, 0.24f, 0.5f });
			gfx::fill(r, c);
		}
		float mx, my;
		mouse(mx, my);
		int hover = hovered(this) && !drag_.from ? itemAt(mx, my) : -1;
		if (hover >= 0 && items[hover].pending) { hover = -1; }
		const float px = std::max(1.0f, std::round(scale_));
		for (size_t i = 0; i < items.size(); i++)
		{
			const GridItem& it = items[i];
			const Rect r = itemRect(it);
			if (drag_.from == this && drag_.index == (int)i && drag_.part())	// the rest of a part carried
			{
				GridItem rest = it;
				rest.count = shownCount((int)i);
				drawItem(rest, r, 1, it.flipH != mirror, true);
			}
			else { drawItem(it, r, itemAlpha((int)i), it.flipH != mirror); }
			if ((int)i == hover) { gfx::fill(r.inset(px), Color{ 1, 1, 1, 0.08f }); }
			if ((int)i == selected) { gfx::frame(r, 2 * px, res::colour("Accent", { 0.85f, 0.64f, 0.25f, 1 })); }
		}
		if (clip)
		{
			gfx::popClip();
			paintBar();
		}
	}

	void ItemGrid::paintBar() const
	{
		const Rect b = bar();
		const float a = std::round(kArrowH * scale_);
		float mx, my;
		mouse(mx, my);
		const bool hot = hovered(this) && b.contains(mx, my);
		const Rect up = { b.x, b.y, b.w, a }, down = { b.x, b.bottom() - a, b.w, a }, t = thumb();
		style().draw(Look::kScrollTrack, { b.x, b.y + a, b.w, b.h - 2 * a }, scale_, State::kNormal, false);
		style().draw(Look::kScrollUp, up, scale_, hot && up.contains(mx, my) ? State::kHighlighted : State::kNormal, false);
		style().draw(Look::kScrollDown, down, scale_, hot && down.contains(mx, my) ? State::kHighlighted : State::kNormal, false);
		style().draw(Look::kScrollThumb, t, scale_, thumbGrab_ >= 0 ? State::kPushed : hot && t.contains(mx, my) ? State::kHighlighted : State::kNormal, false);
	}

	bool findSpace(int cols, int rows, const std::vector<GridItem>& items, int w, int h, int& x, int& y)
	{
		for (y = 0; y + h <= rows; y++)
		{
			for (x = 0; x + w <= cols; x++)
			{
				if (std::none_of(items.begin(), items.end(), [&](const GridItem& o) { return o.x >= 0 && overlaps(o, x, y, w, h); })) { return true; }
			}
		}
		x = y = -1;
		return false;
	}

	bool arrange(int cols, int rows, std::vector<GridItem>& items)
	{
		std::vector<size_t> order(items.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].w * items[a].h > items[b].w * items[b].h; });
		for (GridItem& it : items) { it.x = it.y = -1; }
		bool all = true;
		for (size_t i : order)
		{
			int x, y;
			all &= findSpace(cols, rows, items, items[i].w, items[i].h, x, y);
			items[i].x = x;
			items[i].y = y;
		}
		return all;
	}
}
