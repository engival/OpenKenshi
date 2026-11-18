#include "ui/gear.hpp"

#include <algorithm>
#include <cctype>

#include "app/config.hpp"
#include "ui/icons.hpp"
#include "ui/trade.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kGap = 12;		// between windows, units
		constexpr float kArrangeW = 100, kArrangeH = 29;	// Kenshi_InventoryCharacterWindow's ARRANGE
		constexpr float kBoxGap = 10, kLimbGap = 33;	// grid to ARRANGE; arms to legs (Kenshi_LimbReplacementWindow)
		// The character window's lattice (Kenshi_InventoryCharacterWindow measured in cells of 25): every slot has its caption above it, and
		// the next slot down starts one caption and kStep below the last, so with the game's slot sizes the shirt and the main inventory share
		// a top edge and the boots and armour a bottom one.
		constexpr float kCap = 22, kStep = 8;
		// The container shows at most this many rows and scrolls the rest: with the money box and ARRANGE the window then stays within the
		// original trader window's 407 at 720 lines, 610 units (kenshi-inspect ui_captures/trade_window.md 1).
		constexpr int kContainerRows = 19;
		constexpr float kColGap = 50;	// two cells between the clothing columns
		// The open windows' shared top, units from the screen's top: the original's trader window Root rect sits at 36 of 720 lines (kinspect
		// ui_captures/trade_window.md, trade_open.png), so the tallest (610) ends above the HUD money box (748). Deferred: -1 centres them.
		const cfg::Int kGearTop(cfg::Group{ "Inventory windows" }, "ui.gear_top", 54, -1, 400, "Inventory windows' top edge, units at 1080 lines; -1 centres them");
	}

	ContainerWindow::ContainerWindow(Widget& screen, std::function<void()> onClose) : screen_(&screen)
	{
		using A = Widget::Align;
		win_ = &screen.add<CaptionWindow>("", std::move(onClose), 12);
		win_->halign = win_->valign = A::kCenter;
		win_->visible = false;
		title_ = &win_->caption();
		Box& contBox = win_->body().add<Box>(true, kBoxGap);
		Panel& type = contBox.add<Panel>(Look::kField, 6);	// TypeText (Kenshi_GenericTextBoxFlat) over the grid
		type.halign = A::kStretch;
		type_ = &type.add<Label>("", "Kenshi_StandardFont_Medium");
		type_->colour = style().textColour(Look::kField, State::kNormal, false);
		type_->halign = A::kStretch;
		type_->align = TextAlign::kCenter;
		Box& trader = contBox.add<Box>(true, 6);
		research_ = &trader.add<Button>("SHOW RESEARCH");	// InventoryResearchWindow's ResearchButton (Kenshi_Button1), the window's width over the grid
		research_->halign = A::kStretch;
		research_->onClicked = [this] { if (auto f = onResearch) { f(); } };
		Panel& money = trader.add<Panel>(Look::kField, 6);	// the original's datapanel (Kenshi_GenericTextBoxFlatSkin), top left
		money_ = &money.add<Label>("", "Kenshi_StandardFont_Medium");
		money_->colour = style().textColour(Look::kField, State::kNormal, false);
		Box& group = trader.add<Box>(true, 2);
		grid_ = &group.add<ItemGrid>(0, 0);
		grid_->viewRows = kContainerRows;
		cap_ = &contBox.add<Label>("", "Kenshi_StandardFont_Medium");	// CapacityText; the layout lays it over ARRANGE's box (?), ours sits above it
		cap_->halign = A::kStretch;
		cap_->align = TextAlign::kCenter;
		cap_->wrap = true;
		arrange_ = &contBox.add<Button>("ARRANGE");
		arrange_->rect = { 0, 0, kArrangeW, kArrangeH };
		arrange_->halign = A::kCenter;
		arrange_->onClicked = [this] { if (auto f = onArrange) { f(); } };
		last_.owner = 1;	// differs from the first state, so update() fills everything
		update({});
	}

	ContainerWindow::~ContainerWindow()
	{
		if (ready() && screen_) { screen_->remove(win_); }
	}

	void ContainerWindow::update(const ContainerState& s)
	{
		if (s == last_) { return; }
		if (s.owner != last_.owner || s.name != last_.name || s.grid.rows != last_.grid.rows) { grid_->scroll = 0; }
		if (s.pinned != last_.pinned) { win_->pinButton().checked = s.pinned; }
		last_ = s;
		title_->text = s.name.empty() ? "CONTAINER" : s.name;
		if (s.trade.active) { for (char& c : title_->text) { c = (char)toupper((unsigned char)c); } }
		title_->colour = s.foreign && !s.trade.active ? res::colour("BadBright") : res::colour("Main");
		money_->parent()->visible = s.trade.active;
		money_->text = "Money: " + trade::cats(s.trade.cats);
		research_->visible = s.kind == ContainerKind::kResearch;
		type_->text = s.type;
		type_->parent()->visible = !s.type.empty();
		cap_->text = s.capacity;
		cap_->visible = !s.capacity.empty();
		win_->closeButton().visible = s.kind != ContainerKind::kFixed;	// Kenshi_WindowC
		ItemGrid& grid = *grid_;
		const GearGridState& gs = s.grid;
		const uint32_t sel = grid.selected >= 0 && grid.selected < (int)grid.items.size() ? grid.items[grid.selected].id : 0;
		grid.cols = gs.cols;
		grid.rows = gs.rows;
		grid.single = gs.single;
		grid.blocked = gs.blocked;
		grid.accepts = gs.accepts;
		grid.items = gs.items;
		grid.visible = gs.cols > 0 && gs.rows > 0;
		auto it = std::find_if(gs.items.begin(), gs.items.end(), [&](const GridItem& i) { return sel && i.id == sel; });
		grid.selected = it == gs.items.end() ? -1 : (int)(it - gs.items.begin());
	}

	int gearTop() { return kGearTop.get(); }

	CaptionWindow& Gear::addWindow(Label*& title, std::function<void()> close)
	{
		using A = Widget::Align;
		CaptionWindow& w = screen_->add<CaptionWindow>("", std::move(close), 12);
		w.halign = w.valign = A::kCenter;
		w.visible = false;
		title = &w.caption();
		return w;
	}

	ItemGrid& Gear::addGrid(Box& parent, const char* caption, int g)
	{
		using A = Widget::Align;
		Box& group = parent.add<Box>(true, 2);
		groups_[g] = &group;
		if (caption)
		{
			Label& l = group.add<Label>(caption, "Kenshi_PaintedTextFont_Medium");
			l.colour = res::colour("Main").alpha(0.7f);
			l.halign = A::kCenter;
		}
		ItemGrid& grid = group.add<ItemGrid>(0, 0);
		wire(grid, g);
		return grid;
	}

	ItemGrid& Gear::addSlot(Widget& canvas, const char* caption, int g)
	{
		Label& l = canvas.add<Label>(caption, "Kenshi_PaintedTextFont_Medium");
		l.colour = res::colour("Main").alpha(0.7f);
		l.fit = false;
		l.align = TextAlign::kCenter;
		caps_[g] = &l;
		ItemGrid& grid = canvas.add<ItemGrid>(0, 0);
		wire(grid, g);
		return grid;
	}

	void Gear::wire(ItemGrid& grid, int g)
	{
		grids_[g] = &grid;
		grid.tag = base_ + g;
		grid.onDrop = [this, g](ItemGrid& from, const GridItem& it, int x, int y)
		{
			// The original's move sounds (audio_sfx_triggers.md 5): into the character's own grids Item_Lift (buying too: the original never posts
			// Buy_Item; a container's Sell_Item / Item_Drop are ItemWindows'). Posted at the drop; the host may still refuse it.
			sound("Item_Lift", "Inventory_Items", it.soundSwitch);
			if (act_.move) { act_.move(last_.owner, from.tag, it, base_ + g, x, y); }
		};
		// The original's transfer messages (kenshi-inspect inv_click10_static.md 2.6, codes 2 and 5): a slot that will not take it, or no room.
		grid.onRefused = [&grid](ItemGrid&, const GridItem& it) { message(grid.blocked || !grid.takes(it) ? "Can't wear that item." : "No room for that item."); };
		grid.onItemClick = [this, g](int i, Btn b)
		{
			select(g, i);
			float mx, my;
			mouse(mx, my);
			if (b != Btn::kRight) { return; }
			if (mods() & SAPP_MODIFIER_CTRL) { if (act_.moveAll) { act_.moveAll(last_.owner, base_ + g, grids_[g]->items[i]); } }	// Ctrl wins over Shift
			else if (act_.context) { act_.context(last_.owner, base_ + g, grids_[g]->items[i], mx, my); }
		};
		grid.onItemActivate = [this, g](int i)
		{
			select(g, i);
			if (act_.activate) { act_.activate(last_.owner, base_ + g, grids_[g]->items[i]); }
		};
		grid.onItemHold = [this, g](int i)
		{
			select(g, i);
			if (act_.hold) { act_.hold(last_.owner, base_ + g, grids_[g]->items[i]); }
		};
		grid.onDropOutside = [this, g](const GridItem& it, float, float)
		{
			sound("Character_Drop_Ground", "Inventory_Items", it.soundSwitch);
			if (act_.dropOutside) { act_.dropOutside(last_.owner, base_ + g, it); }
		};
	}

	Gear::Gear(GearActions actions, int tagBase) : act_(std::move(actions)), base_(tagBase)
	{
		using A = Widget::Align;
		Widget& screen = root().add<Widget>();
		screen_ = &screen;
		screen.halign = screen.valign = A::kStretch;
		const Color dim = res::colour("Main").alpha(0.7f);

		// The character window.
		char_ = &addWindow(title_, [this] { if (act_.close) { auto f = act_.close; f(); } else { show(false); } });
		char_->pinButton().visible = true;	// the original's Kenshi_PinButtonSkin (kinspect loot.md): the host keeps a pinned window's owner
		empty_ = &char_->body().add<Label>("No inventory received yet");
		empty_->halign = empty_->valign = A::kCenter;
		Widget& body = char_->body().add<Widget>();
		body_ = &body;
		bagButton_ = &body.add<Button>("OPEN BAG");
		bagButton_->tooltip = "The backpack's contents";
		bagButton_->onClicked = [this] { showBag(!bag_); };
		limbsButton_ = &body.add<Button>("LIMBS");
		limbsButton_->tooltip = "Robotic limbs";
		limbsButton_->onClicked = [this] { showLimbs(!limbs_); };
		addSlot(body, "-BACKPACK-", kGearBackpack);
		addSlot(body, "WEAPON I", kGearWeapon1);
		addSlot(body, "WEAPON II", kGearWeapon2);
		addSlot(body, "BELT", kGearBelt);
		addSlot(body, "SHIRT", kGearShirt);
		addSlot(body, "PANTS", kGearPants);
		addSlot(body, "BOOTS", kGearBoots);
		addSlot(body, "HEAD", kGearHead);
		addSlot(body, "ARMOR", kGearArmour);
		infoPanel_ = &body.add<Panel>(Look::kInset, 10);
		infoRows_ = &infoPanel_->add<Box>(true, 2);
		addSlot(body, "INVENTORY", kGearMain);
		arrangeMain_ = &body.add<Button>("ARRANGE");
		arrangeMain_->onClicked = [this] { if (act_.arrange) { act_.arrange(last_.owner, base_ + kGearMain); } };

		// The backpack's contents.
		bagWin_ = &addWindow(bagTitle_, [this] { showBag(false); });
		Box& bagBox = bagWin_->body().add<Box>(true, kBoxGap);
		addGrid(bagBox, nullptr, kGearBag);
		arrangeBag_ = &bagBox.add<Button>("ARRANGE");
		arrangeBag_->rect = { 0, 0, kArrangeW, kArrangeH };
		arrangeBag_->halign = A::kCenter;
		arrangeBag_->onClicked = [this] { if (act_.arrange) { act_.arrange(last_.owner, base_ + kGearBag); } };

		// Robotics: arm slots over leg slots, either side of the body figure.
		Label* limbTitle = nullptr;
		limbWin_ = &addWindow(limbTitle, [this] { showLimbs(false); });
		limbTitle->text = "ROBOTICS";
		Box& limbs = limbWin_->body().add<Box>(true, 4);
		Label& arms = limbs.add<Label>("-ARMS-", "Kenshi_PaintedTextFont_Medium");
		arms.colour = dim;
		arms.halign = A::kCenter;
		Panel& doll = limbs.add<Panel>(Look::kNone);
		Picture& figure = doll.add<Picture>(res::file("gfx/figure.png"));
		figure.halign = figure.valign = A::kStretch;
		figure.tint = style().colour("Figure", Color{}.alpha(0.6f));	// the original keeps figure.png's own brown
		// Kenshi_LimbReplacementWindow: the slots in the figure's corners, 66 units apart across, 33 between arms and legs.
		Box& limbSides = doll.add<Box>(true, kLimbGap);
		Box& armRow = limbSides.add<Box>(false, 66);
		addGrid(armRow, nullptr, kGearArmRight);
		addGrid(armRow, nullptr, kGearArmLeft);
		Box& legRow = limbSides.add<Box>(false, 66);
		addGrid(legRow, nullptr, kGearLegRight);
		addGrid(legRow, nullptr, kGearLegLeft);
		Label& legs = limbs.add<Label>("-LEGS-", "Kenshi_PaintedTextFont_Medium");
		legs.colour = dim;
		legs.halign = A::kCenter;

		for (CaptionWindow* w : { bagWin_, limbWin_ }) { w->follow(*char_); }	// one group with a partner's container (user 2026-11-05; the original moves each)

		last_.valid = true;	// differs from the first state, so update() fills everything
		update({});
		rebuildInfo();
	}

	Gear::~Gear()
	{
		if (partner_) { partner_->window().unfollow(); }
		if (ready() && screen_) { root().remove(screen_); }
	}

	bool Gear::shown() const { return shown_; }
	bool Gear::pinned() const { return char_->visible && char_->pinned(); }
	Widget* Gear::pinButton() const { return &char_->pinButton(); }

	void Gear::show(bool on)
	{
		if (on && !shown_) { screen_->raise(); }
		if (!on) { char_->pinButton().checked = false; }	// closing unpins (ours; deferred: playtesting)
		if (on != shown_) { sound(on ? "Inventory_Open" : "Inventory_Close"); }
		shown_ = on;
		place();
	}

	void Gear::showBag(bool on)
	{
		bag_ = on;
		place();
	}

	void Gear::showLimbs(bool on)
	{
		limbs_ = on;
		place();
	}

	void Gear::dock(GearDock d)
	{
		if (d == dock_) { return; }
		dock_ = d;
		place();
	}

	void Gear::offset(float x, float y)
	{
		if (x == offX_ && y == offY_) { return; }
		offX_ = x;
		offY_ = y;
		place();
	}

	Widget* Gear::arrangeButton(int g) const
	{
		if (g == kGearContainer) { return partner_ ? partner_->arrangeButton() : nullptr; }
		return g == kGearMain ? arrangeMain_ : g == kGearBag ? arrangeBag_ : nullptr;
	}

	void Gear::partner(ContainerWindow* c)
	{
		if (c == partner_) { return; }
		if (partner_) { partner_->window().unfollow(); }
		partner_ = c;
		if (c) { c->window().follow(*char_); }
		place();
	}

	Widget* Gear::window(int g) const
	{
		if (g == kGearBag) { return bagWin_; }
		if (g == kGearContainer) { return partner_ ? &partner_->window() : nullptr; }
		if (g >= kGearArmRight && g <= kGearLegLeft) { return limbWin_; }
		return g >= 0 && g < kGearGrids ? char_ : nullptr;
	}

	void Gear::select(int g, int i)
	{
		for (ItemGrid* grid : grids_) { if (grid) { grid->selected = -1; } }
		selGrid_ = g;
		selId_ = grids_[g]->items[i].id;
		grids_[g]->selected = i;
	}

	void Gear::rebuildInfo()
	{
		using A = Widget::Align;
		infoRows_->clear();
		info_.clear();
		const float w = infoPanel_->rect.w - 20;
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);	// dark on the original's parchment
		const Color off = style().textColour(Look::kInset, State::kDisabled, false);
		for (const GearInfo& line : last_.info)
		{
			Widget& row = infoRows_->add<Widget>();
			row.rect = { 0, 0, w, 24 };
			row.tooltip = line.tip;
			row.solid = !line.tip.empty();
			Label& l = row.add<Label>(line.label, "Kenshi_StandardFont_Medium");
			l.valign = A::kCenter;
			l.colour = line.dim ? off : ink;
			Label& v = row.add<Label>(line.value, "Kenshi_StandardFont_Medium");
			v.halign = A::kEnd;
			v.valign = A::kCenter;
			v.colour = line.dim ? off : line.colour.empty() ? ink : res::colour(line.colour, ink);
			info_.push_back(&v);
		}
		infoPanel_->visible = !last_.info.empty();
	}

	void Gear::layoutChar()
	{
		auto size = [this](int g) { return grids_[g]->preferred(); };
		// The slot at (x, y) with its caption above, centred over capW (0: the slot); returns its bottom. An absent slot takes no room.
		auto put = [&](int g, float x, float y, float capW = 0)
		{
			grids_[g]->rect.x = x;
			grids_[g]->rect.y = y;
			caps_[g]->rect = { x, y - kCap, capW > 0 ? capW : size(g).w, kCap };
			return grids_[g]->cols > 0 && grids_[g]->rows > 0 ? y + size(g).h : y - kCap - kStep;
		};
		auto below = [](float bottom) { return bottom + kStep + kCap; };	// the next slot's top under one ending at bottom
		const float cell = grids_[kGearMain]->cell;
		const float x0 = 4;

		// OPEN BAG / LIMBS beside the worn backpack.
		bagButton_->rect = { 0, 0, 116, 48 };
		limbsButton_->rect = { 0, 50, 116, 48 };
		const float bagBottom = put(kGearBackpack, 132, kCap);

		// Two clothing columns: shirt, pants, boots on the left; belt, head, armour on the right. The weapons span both.
		const float colL = std::max({ size(kGearShirt).w, size(kGearPants).w, size(kGearBoots).w, 4 * cell });
		const float colRx = x0 + colL + kColGap;
		const float colR = std::max({ size(kGearHead).w, size(kGearArmour).w, size(kGearBelt).w, 4 * cell });
		const float w1 = put(kGearWeapon1, x0, std::max(below(98), bagBottom + 12), 4 * cell);
		const float w2 = put(kGearWeapon2, x0, below(w1), 4 * cell);
		const float belt = put(kGearBelt, colRx + colR - size(kGearBelt).w, below(w1));
		const float shirtY = below(w2);
		const float boots = put(kGearBoots, x0, below(put(kGearPants, x0, below(put(kGearShirt, x0, shirtY)))));
		const float armour = put(kGearArmour, colRx, below(put(kGearHead, colRx, below(std::max(w2, belt)))));

		// The main inventory beside them, level with the shirt; the stats panel fills the space above it; ARRANGE under it.
		const float mainX = colRx + colR + 12;
		const float mainBottom = put(kGearMain, mainX, shirtY);
		const float mainW = std::max(size(kGearMain).w, 8 * cell);
		infoPanel_->rect = { mainX, 0, mainW, shirtY - kCap - kStep };
		arrangeMain_->rect = { mainX + (mainW - 100) / 2, mainBottom + kStep, 100, 29 };
		body_->rect = { 0, 0, std::max(mainX + mainW, x0 + std::max(size(kGearWeapon1).w, colRx + colR - x0)) + 4,
			std::max({ boots, armour, mainBottom + kStep + 29 }) + 4 };
	}

	void Gear::place()
	{
		const GearState& s = last_;
		const bool on = shown_ && s.valid;
		const GearGridState& bag = s.grids[kGearBag];
		char_->visible = on;
		bagWin_->visible = char_->visible && bag_ && bag.cols > 0 && bag.rows > 0;
		limbWin_->visible = char_->visible && limbs_;
		CaptionWindow* const cont = partner_ ? &partner_->window() : nullptr;	// shown by its owner (ItemWindows)
		// Alone the character window stays centred; the backpack opens on its left, robotics and the container on its right. Docked beside a
		// body's the two character windows meet at the centre and every side window opens outwards. As in the game (kinspect
		// darkui_robotics_tooltip_0948.jpg), the open windows share their tops, each at its own height: at ui.gear_top, or centred on the tallest.
		CaptionWindow* const sides[] = { bagWin_, limbWin_, cont };	// cont may be null
		const int top = kGearTop.get();
		const float ch = char_->preferred().h;
		float h = char_->visible ? ch : 0;
		for (CaptionWindow* w : sides) { if (w && w->visible) { h = std::max(h, w->preferred().h); } }
		for (CaptionWindow* w : { char_, bagWin_, limbWin_, cont })
		{
			if (!w) { continue; }
			w->valign = top < 0 ? Widget::Align::kCenter : Widget::Align::kStart;
			w->rect.y = top < 0 ? -(h - w->preferred().h) / 2 : top;
		}
		const float cw = char_->preferred().w;
		char_->rect.x = dock_ == GearDock::kLeft ? -(cw + kGap) / 2 : dock_ == GearDock::kRight ? (cw + kGap) / 2 : 0;
		float left = char_->rect.x - cw / 2 - kGap, right = char_->rect.x + cw / 2 + kGap;
		for (CaptionWindow* w : sides)
		{
			if (!w || !w->visible) { continue; }
			const float ww = w->preferred().w;
			if (dock_ == GearDock::kLeft || (dock_ == GearDock::kCentre && w == bagWin_))
			{
				w->rect.x = left - ww / 2;
				left -= ww + kGap;
			}
			else
			{
				w->rect.x = right + ww / 2;
				right += ww + kGap;
			}
		}
		for (CaptionWindow* w : { char_, bagWin_, limbWin_, cont })
		{
			if (!w) { continue; }
			w->rect.x += offX_;
			w->rect.y += offY_;
		}
	}

	void Gear::update(const GearState& s)
	{
		claimTips(infoRows_, fixedTips());
		if (s == last_)
		{
			place();
			return;
		}
		const bool infoChanged = s.info != last_.info || s.grids[kGearMain].cols != last_.grids[kGearMain].cols;
		last_ = s;
		empty_->visible = !s.valid;
		body_->visible = s.valid;
		title_->text = s.name.empty() ? "INVENTORY" : s.name;
		title_->colour = s.nameForeign ? res::colour("BadBright") : res::colour("Main");
		bagTitle_->text = s.bagName.empty() ? "BACKPACK" : s.bagName;
		for (int g = 0; g < kGearGrids; g++)
		{
			if (!grids_[g]) { continue; }	// kGearContainer: ItemWindows' container window
			const GearGridState& gs = s.grids[g];
			ItemGrid& grid = *grids_[g];
			grid.cols = gs.cols;
			grid.rows = gs.rows;
			grid.single = gs.single;
			grid.blocked = gs.blocked;
			grid.accepts = gs.accepts;
			grid.items = gs.items;
			grid.mirror = (s.mirror == LimbMirror::kLeft && (g == kGearArmLeft || g == kGearLegLeft))
				|| (s.mirror == LimbMirror::kRight && (g == kGearArmRight || g == kGearLegRight));
			if (groups_[g]) { groups_[g]->visible = gs.cols > 0 && gs.rows > 0; }
			if (caps_[g]) { caps_[g]->visible = grid.visible = gs.cols > 0 && gs.rows > 0; }
			grid.selected = -1;
			if (g == selGrid_)
			{
				auto it = std::find_if(gs.items.begin(), gs.items.end(), [&](const GridItem& i) { return i.id == selId_; });
				grid.selected = it == gs.items.end() ? -1 : (int)(it - gs.items.begin());
			}
		}
		bagButton_->enabled = s.grids[kGearBag].cols > 0 && s.grids[kGearBag].rows > 0;
		layoutChar();
		if (infoChanged) { rebuildInfo(); }
		place();
	}
}
