#include "ui/itemwindows.hpp"

#include <algorithm>
#include <cmath>

#include "app/config.hpp"
#include "ui/font.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const cfg::Bool kBadges{ cfg::Group{ "Inventory windows" }, "ui.cursor_badges", true, "Item windows: a small badge by the pointer over an item says what a right click does (buy, sell, move; Shift: the stack, Ctrl: all)" };
		const cfg::Bool kPlaceOne{ cfg::Group{ "Inventory windows" }, "ui.place_one", false, "Item windows: a lifted stack is placed one unit per click, the rest stays lifted (the original's setting)" };
	}

	// ---- ItemMenu ----

	ItemMenu::ItemMenu(const std::string& title, const std::vector<ItemMenuRow>& rows, std::function<void(int)> onPick) : Menu({})
	{
		keepOnMiss = true;
		items.push_back({ title, {}, true, true });
		bindings.push_back({});
		for (const ItemMenuRow& r : rows)
		{
			const int a = r.action;
			items.push_back({ r.label, [onPick, a] { if (onPick) { onPick(a); } }, r.enabled });
			bindings.push_back(r.binding);
		}
	}

	Size ItemMenu::preferred() const
	{
		Size p = Menu::preferred();
		if (!font) { return p; }
		float b = 0;
		for (const std::string& t : bindings) { b = std::max(b, font->width(t)); }
		if (b > 0) { p.w += std::ceil(b / scale()) + 24; }
		return p;
	}

	Rect ItemMenu::row(size_t i) const
	{
		const Rect c = content();
		const float rowH = font ? font->lineHeight() + 6 * scale_ : 0;
		return { c.x, c.y + i * rowH, c.w, rowH };
	}

	void ItemMenu::paint() const
	{
		Menu::paint();
		if (!font) { return; }
		const Color main = style().textColour(Look::kMenu, State::kNormal, false);
		for (size_t i = 0; i < items.size() && i < bindings.size(); i++)
		{
			if (bindings[i].empty()) { continue; }
			const Rect r = row(i);
			font->draw(std::round(r.right() - 8 * scale_ - font->width(bindings[i])), std::round(r.y + 3 * scale_), bindings[i], main.alpha(items[i].enabled ? 0.55f : 0.3f));
		}
	}

	// ---- the badge ----

	// Drawn last over everything, just below-right of the pointer (our own drawing in both styles): kBuy a coin with a plus, kSell a coin with
	// a minus, kTransfer two opposed arrows; Shift adds a pip of two squares (the stack), Ctrl one of three (all).
	class ItemWindows::Badge : public Widget
	{
	public:
		explicit Badge(const ItemWindows& w) : w_(w) { halign = valign = Align::kStretch; }

	protected:
		void paint() const override
		{
			const RightVerb v = w_.badge();
			if (v == RightVerb::kNone || v == RightVerb::kMenu) { return; }
			float mx, my;
			mouse(mx, my);
			const float s = scale_, d = std::round(18 * s), x = std::round(mx + 14 * s), y = std::round(my + 18 * s);
			const Color ink = res::colour("Main"), back = Color{ 0, 0, 0, 0.75f }, gold = res::colour("Title", ink);
			gfx::fill({ x - 2 * s, y - 2 * s, d + 4 * s, d + 4 * s }, back);
			const float cx = x + d / 2, cy = y + d / 2, t = std::max(1.0f, std::round(2 * s));
			if (v == RightVerb::kTransfer)
			{
				const float a = d * 0.42f, h = d * 0.18f;
				gfx::fill({ cx - a, cy - h - t / 2, 2 * a - h, t }, ink);	// right, on top
				gfx::triangle(cx + a, cy - h, cx + a - 2 * h, cy - 2.6f * h, cx + a - 2 * h, cy + 0.6f * h, ink);
				gfx::fill({ cx - a + h, cy + h - t / 2, 2 * a - h, t }, ink);	// left, below
				gfx::triangle(cx - a, cy + h, cx - a + 2 * h, cy - 0.6f * h + 2 * h - h, cx - a + 2 * h, cy + 2.6f * h, ink);
			}
			else
			{
				const float r = d * 0.45f;
				constexpr int kSides = 20;
				for (int i = 0; i < kSides; i++)
				{
					const float p = i * 6.2831853f / kSides, q = (i + 1) * 6.2831853f / kSides;
					gfx::triangle(cx, cy, cx + r * std::cos(p), cy + r * std::sin(p), cx + r * std::cos(q), cy + r * std::sin(q), gold);
				}
				const float a = r * 0.55f;
				gfx::fill({ std::round(cx - a), std::round(cy - t / 2), std::round(2 * a), t }, back);
				if (v == RightVerb::kBuy) { gfx::fill({ std::round(cx - t / 2), std::round(cy - a), t, std::round(2 * a) }, back); }
			}
			const uint32_t m = w_.badgeMods();
			const int pips = (m & SAPP_MODIFIER_CTRL) ? 3 : (m & SAPP_MODIFIER_SHIFT) ? 2 : 0;
			const float p = std::round(4 * s);
			for (int i = 0; i < pips; i++) { gfx::fill({ x + d + 4 * s, y + d - (i + 1) * (p + s), p, p }, ink); }
		}

	private:
		const ItemWindows& w_;
	};

	ItemWindows::ItemWindows(ItemWindowsActions actions) : act_(std::move(actions))
	{
		badgeW_ = &root().add<Badge>(*this);
	}

	ItemWindows::~ItemWindows()
	{
		chars_.clear();	// the docked container follows the first character window: that one goes first
		for (Cont& c : conts_) { dropCont(c); }
		conts_.clear();
		if (ready() && badgeW_) { root().remove(badgeW_); }
	}

	ItemMenu* ItemWindows::menu() const { return popupsOpen() ? menu_ : nullptr; }

	void ItemWindows::openMenu(GridRef at, const GridItem& item, bool held)
	{
		std::vector<ItemMenuRow> rows = act_.menu ? act_.menu(at, item) : std::vector<ItemMenuRow>{};
		if (rows.empty()) { return; }
		float mx, my;
		mouse(mx, my);
		auto m = std::make_unique<ItemMenu>(item.name, rows, [this, at, item](int a) { if (auto f = act_.pick) { f(at, item, a); } });
		menu_ = m.get();
		ui::openMenu(std::move(m), mx, my, held);
	}

	void ItemWindows::rightClick(GridRef at, const GridItem& item)
	{
		const uint32_t m = mods();
		if (act_.verb && act_.verb(at, item, m) == RightVerb::kMenu)
		{
			openMenu(at, item, false);
			return;
		}
		if ((m & SAPP_MODIFIER_CTRL) && act_.moveAll) { act_.moveAll(at, item); }	// Ctrl wins over Shift
		else if (auto f = act_.quick) { f(at, item, m); }
	}

	void ItemWindows::hover()
	{
		badge_ = RightVerb::kNone;
		badgeMods_ = mods();
		if (!kBadges.get() || !act_.verb || ItemGrid::dragged() || popupsOpen()) { return; }
		float mx, my;
		mouse(mx, my);
		auto look = [&](ItemGrid* g)
		{
			if (!g || !g->shown() || !hovered(g)) { return false; }
			const int i = g->itemAt(mx, my);
			if (i < 0 || g->items[i].pending) { return true; }
			badge_ = act_.verb(ref(g->tag), g->items[i], badgeMods_);
			return true;
		};
		for (const Char& c : chars_) { for (int g = 0; g < kGearGrids; g++) { if (look(c.gear->grid(g))) { return; } } }
		for (const Cont& c : conts_) { if (look(&c.win->grid())) { return; } }
	}

	void ItemWindows::markButton(Button& b, uint64_t owner)
	{
		b.visible = true;
		b.onClicked = [this, owner] { if (auto f = act_.mark) { f(target_ == owner ? 0 : owner); } };
	}

	std::unique_ptr<Gear> ItemWindows::makeGear(uint64_t owner, int base)
	{
		GearActions a;
		a.move = [this](uint64_t, int from, const GridItem& it, int to, int x, int y) { if (auto f = act_.move) { f(ref(from), it, ref(to), x, y); } };
		a.activate = [this](uint64_t, int g, const GridItem& it) { if (auto f = act_.activate) { f(ref(g), it); } };
		a.context = [this](uint64_t, int g, const GridItem& it, float, float) { rightClick(ref(g), it); };
		a.moveAll = [this](uint64_t, int g, const GridItem& it) { rightClick(ref(g), it); };
		a.hold = [this](uint64_t, int g, const GridItem& it) { openMenu(ref(g), it, true); };
		a.dropOutside = [this](uint64_t, int g, const GridItem& it) { if (auto f = act_.dropOutside) { f(ref(g), it); } };
		a.arrange = [this](uint64_t, int g) { if (auto f = act_.arrange) { f(ref(g)); } };
		a.close = [this, owner] { if (auto f = act_.close) { f(owner); } };
		auto gear = std::make_unique<Gear>(std::move(a), base);
		CaptionWindow& w = gear->characterWindow();
		w.pinButton().onToggle = [this, owner](bool on) { if (auto f = act_.pin) { f(owner, on); } };
		markButton(w.targetButton(), owner);
		stackKeys(w);
		gear->show(true);
		return gear;
	}

	void ItemWindows::stackKeys(CaptionWindow& w)
	{
		w.onEscape = [this] { if (const uint64_t o = escapee()) { if (auto f = act_.close) { f(o); } } };
		w.takesEscape = [this] { return escapee() != 0; };
	}

	uint64_t ItemWindows::escapee() const
	{
		for (auto it = order_.rbegin(); it != order_.rend(); ++it)
		{
			const CaptionWindow* w = window(*it);
			if (w && w->visible && !w->pinned()) { return *it; }
		}
		return 0;
	}

	uint64_t ItemWindows::top() const
	{
		for (auto it = order_.rbegin(); it != order_.rend(); ++it)
		{
			const CaptionWindow* w = window(*it);
			if (w && w->visible) { return *it; }
		}
		return 0;
	}

	Widget* ItemWindows::layer(uint64_t owner) const
	{
		if (Gear* g = character(owner)) { return &g->layer(); }
		for (const Cont& c : conts_) { if (c.owner == owner) { return c.layer; } }
		return nullptr;
	}

	uint64_t ItemWindows::ownerOf(const Widget* w) const
	{
		while (w && w->parent() && w->parent() != &root()) { w = w->parent(); }
		if (!w) { return 0; }
		for (const Char& c : chars_) { if (&c.gear->layer() == w) { return c.owner; } }
		for (const Cont& c : conts_) { if (c.layer == w) { return c.owner; } }
		return 0;
	}

	void ItemWindows::raise(uint64_t owner)
	{
		Widget* l = layer(owner);
		if (!l) { return; }
		std::erase(order_, owner);
		order_.push_back(owner);
		l->raise();
		badgeW_->raise();
	}

	void ItemWindows::dropCont(Cont& c)
	{
		c.win.reset();
		if (ready() && c.layer) { root().remove(c.layer); }
		c.layer = nullptr;
	}

	std::unique_ptr<ContainerWindow> ItemWindows::makeContainer(uint64_t owner, int base, Widget& layer)
	{
		auto c = std::make_unique<ContainerWindow>(layer, [this, owner] { if (auto f = act_.close) { f(owner); } });
		ContainerWindow& cw = *c;
		cw.onArrange = [this, owner] { if (auto f = act_.arrange) { f({ owner, kGearContainer }); } };
		cw.onResearch = [this, owner] { if (auto f = act_.showResearch) { f(owner); } };
		CaptionWindow& w = cw.window();
		stackKeys(w);
		w.pinButton().visible = true;	// ours (the original pins no world container, kenshi-rwh.78; the user's ask on kenshi-k16.20.13)
		w.pinButton().onToggle = [this, owner](bool on) { if (auto f = act_.pin) { f(owner, on); } };
		markButton(w.targetButton(), owner);
		ItemGrid& grid = cw.grid();
		grid.tag = base + kGearContainer;
		const GridRef at{ owner, kGearContainer };
		grid.onDrop = [this, &cw, at](ItemGrid& from, const GridItem& it, int x, int y)
		{
			// As Gear's (audio_sfx_triggers.md 5): into a trader's a sale, into another container Item_Drop, within it Item_Lift.
			const bool in = from.tag != cw.grid().tag;
			sound(!in ? "Item_Lift" : cw.state().trade.active ? "Sell_Item" : "Item_Drop", "Inventory_Items", it.soundSwitch);
			if (auto f = act_.move) { f(ref(from.tag), it, at, x, y); }
		};
		grid.onRefused = [](ItemGrid&, const GridItem&) { message("No room for that item."); };
		grid.onItemClick = [this, &grid, at](int i, Btn b)
		{
			grid.selected = i;
			if (b == Btn::kRight) { rightClick(at, GridItem(grid.items[i])); }
		};
		grid.onItemHold = [this, &grid, at](int i)
		{
			grid.selected = i;
			openMenu(at, GridItem(grid.items[i]), true);
		};
		grid.onItemActivate = [this, &grid, at](int i)
		{
			grid.selected = i;
			if (auto f = act_.activate) { f(at, grid.items[i]); }
		};
		grid.onDropOutside = [this, at](const GridItem& it, float, float)
		{
			sound("Character_Drop_Ground", "Inventory_Items", it.soundSwitch);
			if (auto f = act_.dropOutside) { f(at, it); }
		};
		return c;
	}

	Gear* ItemWindows::character(uint64_t owner) const
	{
		for (const Char& c : chars_) { if (c.owner == owner) { return c.gear.get(); } }
		return nullptr;
	}

	ContainerWindow* ItemWindows::container(uint64_t owner) const
	{
		for (const Cont& c : conts_) { if (c.owner == owner) { return c.win.get(); } }
		return nullptr;
	}

	CaptionWindow* ItemWindows::window(uint64_t owner) const
	{
		if (Gear* g = character(owner)) { return &g->characterWindow(); }
		ContainerWindow* c = container(owner);
		return c ? &c->window() : nullptr;
	}

	GridRef ItemWindows::ref(int tag) const
	{
		const int base = tag / kTagStep * kTagStep;
		for (const Char& c : chars_) { if (c.base == base) { return { c.owner, tag - base }; } }
		for (const Cont& c : conts_) { if (c.base == base) { return { c.owner, tag - base }; } }
		return {};
	}

	void ItemWindows::update(const ItemWindowsState& s)
	{
		target_ = s.target;
		// A press since the last update put the keyboard on a window below the top (the focus is kept on the top one otherwise): raise it.
		const uint64_t held = ownerOf(focusedWidget());	// the keyboard's window, before any closes
		if (held && held != top()) { raise(held); }
		std::vector<uint64_t> opened;
		// The windows in the state's order: kept by owner, made for a new one, dropped for a gone one.
		std::vector<Cont> conts;
		for (const ContainerState& cs : s.containers)
		{
			auto it = std::find_if(conts_.begin(), conts_.end(), [&](const Cont& c) { return c.owner == cs.owner; });
			if (it != conts_.end())
			{
				conts.push_back(std::move(*it));
				conts_.erase(it);
				continue;
			}
			const int base = nextBase_;
			nextBase_ = nextBase_ >= 1000 * kTagStep ? kTagStep : nextBase_ + kTagStep;
			Widget& layer = root().add<Widget>();
			layer.halign = layer.valign = Widget::Align::kStretch;
			conts.push_back({ cs.owner, base, -1, false, &layer, makeContainer(cs.owner, base, layer) });
			opened.push_back(cs.owner);
		}
		for (Cont& gone : conts_)	// closed: unhook the dock first
		{
			if (gone.owner == docked_)
			{
				for (Char& c : chars_) { c.gear->partner(nullptr); }
				docked_ = 0;
			}
			dropCont(gone);
		}
		conts_ = std::move(conts);
		std::vector<Char> chars;
		for (const GearState& gs : s.characters)
		{
			auto it = std::find_if(chars_.begin(), chars_.end(), [&](const Char& c) { return c.owner == gs.owner; });
			if (it != chars_.end())
			{
				chars.push_back(std::move(*it));
				chars_.erase(it);
				continue;
			}
			const int base = nextBase_;
			nextBase_ = nextBase_ >= 1000 * kTagStep ? kTagStep : nextBase_ + kTagStep;
			chars.push_back({ gs.owner, base, makeGear(gs.owner, base) });
			opened.push_back(gs.owner);
		}
		for (Char& gone : chars_) { gone.gear->partner(nullptr); }
		chars_ = std::move(chars);

		// The dock: a container opened while none is docked docks; one opened beside it cascades one step past the last. A window never
		// changes place on its own once open (a docked one closing leaves the others where they are; the next to open docks).
		for (size_t i = 0; i < conts_.size(); i++)
		{
			Cont& c = conts_[i];
			if (c.cascade < 0)
			{
				int most = 0;
				for (const Cont& o : conts_) { most = std::max(most, o.cascade); }
				c.cascade = docked_ == 0 ? 0 : most + 1;
				if (docked_ == 0) { docked_ = c.owner; }
			}
			c.win->update(s.containers[i]);
			c.win->window().visible = c.win->open();
			c.win->window().targetButton().checked = c.owner == target_;
		}
		ContainerWindow* dock = container(docked_);
		for (size_t i = 0; i < chars_.size(); i++)
		{
			Gear& g = *chars_[i].gear;
			g.partner(i == 0 ? dock : nullptr);
			g.dock(chars_.size() < 2 ? GearDock::kCentre : i == 0 ? GearDock::kLeft : GearDock::kRight);
			g.offset(i < 2 ? 0 : kCascade * (i - 1), i < 2 ? 0 : kCascade * (i - 1));
			g.update(s.characters[i]);
			g.characterWindow().targetButton().checked = chars_[i].owner == target_;
		}
		ItemGrid::placeOne = kPlaceOne.get();
		for (Char& c : chars_) { for (int g = 0; g < kGearGrids; g++) { if (ItemGrid* grid = c.gear->grid(g)) { grid->holdTime = holdTime; } } }
		for (Cont& c : conts_) { c.win->grid().holdTime = holdTime; }
		place();

		// The stack: closed ones out, new ones on top. The keyboard follows the top window when its own closed or hid (the next Escape closes
		// the next one down) and goes to a new one unless something else has it (an edit box); a window the world had it from too.
		std::erase_if(order_, [this](uint64_t o) { return !layer(o); });
		for (uint64_t o : opened) { raise(o); }
		const Widget* f = focusedWidget();
		const bool lost = held && (!f || !f->shown());
		if (lost || (!opened.empty() && (!f || ownerOf(f))))
		{
			const uint64_t t = top();
			focus(t ? window(t) : nullptr);
		}
		hover();
		badgeW_->raise();
	}

	void ItemWindows::place()
	{
		// The docked container: placed by the first character window's Gear; alone, centred at ui.gear_top. The rest: kCascade steps from it
		// when they open, and there they stay.
		const int top = gearTop();
		const ContainerWindow* dock = container(docked_);
		CaptionWindow* lead = chars_.empty() ? nullptr : &chars_.front().gear->characterWindow();
		float x = 0, y = top < 0 ? 0 : top;
		if (dock && !chars_.empty())
		{
			x = dock->window().rect.x;
			y = dock->window().rect.y;
		}
		else if (lead)
		{
			x = lead->rect.x + lead->preferred().w / 2 + 12;
		}
		for (Cont& c : conts_)
		{
			CaptionWindow& w = c.win->window();
			if ((c.owner == docked_ && !chars_.empty()) || (c.owner != docked_ && c.placed)) { continue; }
			c.placed = c.owner != docked_;
			w.valign = top < 0 ? Widget::Align::kCenter : Widget::Align::kStart;
			const float half = c.owner == docked_ || !lead || dock ? 0 : w.preferred().w / 2;	// beside the character window when nothing is docked
			w.rect.x = x + half + kCascade * c.cascade;
			w.rect.y = y + kCascade * c.cascade;
		}
	}
}
