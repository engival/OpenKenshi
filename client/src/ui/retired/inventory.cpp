#include "ui/inventory.hpp"

#include <algorithm>
#include <cstdlib>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// The equipment rows, in the original window's order (Kenshi_InventoryCharacterWindow.layout). Entry slot = 1 wielded, 2 + ARMOUR slot.
		struct SlotRow { int slot; const char* label; };
		const SlotRow kSlots[] = {
			{ 1, "WEAPON" }, { 3, "BACK" }, { 16, "BELT" }, { 5, "HEAD" }, { 10, "SHIRT" }, { 7, "ARMOUR" }, { 8, "LEGS" }, { 11, "BOOTS" }, { 14, "BACKPACK" },
		};

		// Window widths (units): equipment + pack, and with the container column.
		const float kNarrow = 850, kWide = 1290;

		std::string nameOf(const InventoryEntry& e) { return e.name.empty() ? "#" + std::to_string(e.item) : e.name; }
		// The game's item grade colours (kenshi_colours.xml; the flat style has them in its palette); def without a grade.
		Color gradeColour(const InventoryEntry& e, const Color& def)
		{
			if (e.grade < 0 || e.grade > 5) { return def; }
			return res::colour(e.grade == 5 ? "ItemArtifact" : "ItemGrade" + std::to_string(e.grade), def);
		}

		std::string tipOf(const InventoryEntry& e) { return e.quality.empty() ? std::string() : nameOf(e) + "\n" + e.quality; }

		// A list's rows from entries: text, grade colour, quality tooltip.
		void fill(List& l, const std::vector<InventoryEntry>& items)
		{
			l.items.clear();
			l.itemColours.clear();
			l.itemTips.clear();
			for (const InventoryEntry& e : items)
			{
				l.items.push_back(nameOf(e) + (e.count > 1 ? "  x" + std::to_string(e.count) : ""));
				l.itemColours.push_back(gradeColour(e, { 0, 0, 0, 0 }));	// none: the list's own colours
				l.itemTips.push_back(tipOf(e));
			}
			if (items.empty()) { l.items.push_back("(empty)"); }
		}


		// A titled column of a list in a scroll view.
		List& listColumn(Box& col, const char* title, Label** titleOut = nullptr)
		{
			Label& t = col.add<Label>(title, "Kenshi_PaintedTextFont_Medium");
			t.colour = res::colour("Main").alpha(0.7f);
			if (titleOut) { *titleOut = &t; }
			ScrollView& sv = col.add<ScrollView>();
			sv.rect = { 0, 0, 420, 300 };
			List& l = sv.body().add<List>();
			l.halign = Widget::Align::kStretch;
			return l;
		}
	}

	Inventory::Inventory(InventoryActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		Window& w = root().add<Window>();
		window_ = &w;
		w.onEscape = [this] { show(false); };
		w.valign = A::kCenter;
		w.rect = { 60, 0, kNarrow, 660 };
		w.visible = false;
		title_ = &w.add<Label>("INVENTORY", "Kenshi_PaintedTextFont_Large");
		title_->shadow = true;
		title_->halign = A::kCenter;
		empty_ = &w.add<Label>("No inventory received yet");
		empty_->halign = empty_->valign = A::kCenter;
		Button& close = w.add<Button>("CLOSE");
		close.halign = close.valign = A::kEnd;
		close.onClicked = [this] { show(false); };
		body_ = &w.add<Widget>();
		body_->halign = body_->valign = A::kStretch;
		body_->rect = { 0, 56, 0, 56 };
		const Color dim = res::colour("Main").alpha(0.7f);

		// Left: what is worn and wielded. A row selects; double-click or Unequip takes it off.
		Box& left = body_->add<Box>(true, 6);
		left.add<Label>("EQUIPMENT", "Kenshi_PaintedTextFont_Medium").colour = dim;
		for (const SlotRow& r : kSlots)
		{
			Box& row = left.add<Box>(false, 8);
			Label& l = row.add<Label>(r.label, "Kenshi_StandardFont_Medium");
			l.fit = false;
			l.rect = { 0, 0, 90, 36 };
			l.colour = dim;
			Button& slot = row.add<Button>("", Look::kSlot);
			slot.rect = { 0, 0, 250, 36 };
			const int i = (int)slots_.size();
			slot.onClicked = [this, i] { select(Side::kSlot, i); };
			slot.onDoubleClicked = [this, i]
			{
				select(Side::kSlot, i);
				doUnequip();
			};
			Label& item = slot.add<Label>();
			item.fit = false;
			item.halign = item.valign = A::kStretch;
			item.rect = { 8, 0, 8, 0 };
			slots_.push_back({ &slot, &item, {} });
		}
		unequip_ = &left.add<Button>("UNEQUIP");
		unequip_->tooltip = "Take the selected piece off, into the pack";
		unequip_->onClicked = [this] { doUnequip(); };
		extra_ = &left.add<Label>("", "Kenshi_StandardFont_Medium");
		extra_->wrap = true;
		extra_->rect.w = 348;

		// Middle: the pack, the count shared by Drop / Put / Take, and the pack's actions.
		Box& mid = body_->add<Box>(true, 6);
		mid.rect.x = 370;
		pack_ = &listColumn(mid, "PACK");
		pack_->onPick = [this](int i) { select(Side::kPack, i); };
		pack_->onActivate = [this](int i)
		{
			select(Side::kPack, i);
			doEquip();
		};
		Box& count = mid.add<Box>(false, 4);
		Button& minus = count.add<Button>("-");
		minus.rect.w = 40;
		minus.onClicked = [this] { setCount((int64_t)dropCount() - 1); };
		count_ = &count.add<EditBox>();
		count_->rect = { 0, 0, 80, 0 };
		count_->maxBytes = 5;
		count_->onCommit = [this](const std::string& t) { setCount(std::strtoll(t.c_str(), nullptr, 10)); };
		Button& plus = count.add<Button>("+");
		plus.rect.w = 40;
		plus.onClicked = [this] { setCount((int64_t)dropCount() + 1); };
		Button& all = count.add<Button>("ALL");
		all.onClicked = [this] { setCount(maxCount()); };
		countRow_ = { &minus, count_, &plus, &all };
		Box& acts = mid.add<Box>(false, 4);
		equip_ = &acts.add<Button>("EQUIP");
		equip_->tooltip = "Wield or wear the selected weapon or armour";
		equip_->onClicked = [this] { doEquip(); };
		drop_ = &acts.add<Button>("DROP");
		drop_->tooltip = "Drop this many at the character's feet";
		drop_->onClicked = [this]
		{
			const InventoryEntry* e = side_ == Side::kPack ? selectedEntry() : nullptr;
			if (e && act_.drop && dropCount() > 0) { act_.drop(last_.owner, e->item, dropCount()); }
		};
		put_ = &acts.add<Button>("PUT");
		put_->tooltip = "Put this many into the container";
		put_->onClicked = [this] { doTransfer(false); };

		// Right: the open container, shown while the host sends one.
		Box& right = body_->add<Box>(true, 6);
		right.rect.x = 812;
		containerCol_ = &right;
		container_ = &listColumn(right, "CONTAINER", &containerTitle_);
		container_->onPick = [this](int i) { select(Side::kContainer, i); };
		container_->onActivate = [this](int i)
		{
			select(Side::kContainer, i);
			doTransfer(true);
		};
		Box& cacts = right.add<Box>(false, 4);
		take_ = &cacts.add<Button>("TAKE");
		take_->tooltip = "Take this many into the pack";
		take_->onClicked = [this] { doTransfer(true); };
		takeAll_ = &cacts.add<Button>("TAKE ALL");
		takeAll_->onClicked = [this]
		{
			if (act_.takeAll && last_.container.valid) { act_.takeAll(last_.owner, last_.container.id); }
		};
		foreignNote_ = &right.add<Label>("Not yours: taking from it is theft", "Kenshi_StandardFont_Medium");
		foreignNote_->colour = res::colour("BadBright");
		rebuild();
		select(Side::kNone, -1);
	}

	Inventory::~Inventory()
	{
		if (ready() && window_) { root().remove(window_); }
	}

	bool Inventory::shown() const { return window_->visible; }

	void Inventory::show(bool on)
	{
		window_->visible = on;
		if (on)
		{
			window_->raise();
			focus(window_);
		}
		else if (focused(window_)) { focus(nullptr); }
	}

	Widget* Inventory::packRow() const { return pack_; }
	Widget* Inventory::containerRow() const { return container_; }
	Widget* Inventory::countBox() const { return count_; }

	const std::vector<InventoryEntry>* Inventory::listOf(Side side) const
	{
		if (side == Side::kPack) { return &packItems_; }
		if (side == Side::kContainer) { return &last_.container.items; }
		return nullptr;
	}

	const InventoryEntry* Inventory::selectedEntry() const
	{
		const std::vector<InventoryEntry>* l = listOf(side_);
		return l && sel_ >= 0 && sel_ < (int)l->size() ? &(*l)[sel_] : nullptr;
	}

	uint32_t Inventory::maxCount() const
	{
		const InventoryEntry* e = selectedEntry();
		return e ? std::min<uint32_t>(e->count, 65535) : 0;
	}

	uint32_t Inventory::dropCount() const
	{
		const long long n = std::strtoll(count_->text.c_str(), nullptr, 10);
		return (uint32_t)std::clamp<long long>(n, maxCount() > 0 ? 1 : 0, maxCount());
	}

	void Inventory::setCount(int64_t n)
	{
		count_->text = std::to_string(std::clamp<int64_t>(n, maxCount() > 0 ? 1 : 0, maxCount()));
	}

	// One selection across the three columns; the buttons follow it.
	void Inventory::select(Side side, int i)
	{
		const std::vector<InventoryEntry>* l = listOf(side);
		const bool ok = l ? i >= 0 && i < (int)l->size() : side == Side::kSlot && i >= 0 && i < (int)slots_.size() && !slots_[i].items.empty();
		side_ = ok ? side : Side::kNone;
		sel_ = ok ? i : -1;
		pack_->selected = side_ == Side::kPack ? sel_ : -1;
		container_->selected = side_ == Side::kContainer ? sel_ : -1;
		for (size_t s = 0; s < slots_.size(); s++) { slots_[s].button->checked = side_ == Side::kSlot && sel_ == (int)s; }
		setCount(maxCount());	// the whole stack, as the original moves it
		const InventoryEntry* e = selectedEntry();
		for (Widget* w : countRow_) { w->enabled = e != nullptr; }
		const bool pack = side_ == Side::kPack;
		drop_->enabled = pack;
		equip_->enabled = pack && e->equippable;
		put_->enabled = pack && last_.container.valid;
		take_->enabled = side_ == Side::kContainer;
		takeAll_->enabled = last_.container.valid && !last_.container.items.empty();
		unequip_->enabled = side_ == Side::kSlot;
	}

	void Inventory::doEquip()
	{
		const InventoryEntry* e = side_ == Side::kPack ? selectedEntry() : nullptr;
		if (e && e->equippable && act_.equip) { act_.equip(last_.owner, e->item, true); }
	}

	void Inventory::doUnequip()	// the first piece of the row (a row shows several only when the data puts two in one slot)
	{
		if (side_ == Side::kSlot && act_.equip) { act_.equip(last_.owner, slots_[sel_].items.front(), false); }
	}

	void Inventory::doTransfer(bool take)
	{
		const InventoryEntry* e = side_ == (take ? Side::kContainer : Side::kPack) ? selectedEntry() : nullptr;
		if (e && last_.container.valid && act_.transfer && dropCount() > 0) { act_.transfer(last_.owner, last_.container.id, e->item, dropCount(), take); }
	}

	void Inventory::update(const InventoryState& s)
	{
		if (s == last_) { return; }
		// Keep the selection on the same item (the n-th entry of that item in its column) across changes.
		const Side side = side_;
		uint32_t selItem = 0;
		int ordinal = 0;
		if (const InventoryEntry* e = selectedEntry())
		{
			selItem = e->item;
			const std::vector<InventoryEntry>& l = *listOf(side);
			for (int i = 0; i < sel_; i++) { ordinal += l[i].item == selItem; }
		}
		const int selSlot = side == Side::kSlot ? sel_ : -1;
		const bool sameOwner = s.owner == last_.owner;
		const bool sameContainer = s.container.valid && last_.container.valid && s.container.id == last_.container.id;
		const uint32_t keepCount = dropCount();
		last_ = s;
		rebuild();
		int found = -1;
		const std::vector<InventoryEntry>* l = listOf(side);
		if (l && selItem && sameOwner && (side == Side::kPack || sameContainer))
		{
			for (int i = 0; i < (int)l->size(); i++)
			{
				if ((*l)[i].item == selItem && ordinal-- == 0) { found = i; break; }
			}
		}
		if (side == Side::kSlot && sameOwner) { found = selSlot; }
		select(side, found);
		if (found >= 0 && l) { setCount(keepCount); }
	}

	void Inventory::rebuild()
	{
		const InventoryState& s = last_;
		title_->text = s.valid && !s.ownerName.empty() ? "INVENTORY - " + s.ownerName : "INVENTORY";
		empty_->visible = !s.valid;
		body_->visible = s.valid;
		packItems_.clear();
		for (Slot& sl : slots_)
		{
			sl.label->text.clear();
			sl.label->colour = res::colour("Main");
			sl.button->tooltip.clear();
			sl.items.clear();
		}
		std::string extra;
		for (const InventoryEntry& e : s.items)
		{
			if (e.slot == 0)
			{
				packItems_.push_back(e);
				continue;
			}
			size_t row = std::size(kSlots);
			for (size_t i = 0; i < std::size(kSlots); i++) { if (kSlots[i].slot == e.slot) { row = i; } }
			if (row == std::size(kSlots))
			{
				extra += (extra.empty() ? "Also worn: " : ", ") + nameOf(e) + " (slot " + std::to_string(e.slot) + ")";
				continue;
			}
			Slot& sl = slots_[row];
			if (sl.items.empty()) { sl.label->colour = gradeColour(e, res::colour("Main")); }	// the first piece's grade
			sl.label->text += (sl.label->text.empty() ? "" : ", ") + nameOf(e);
			const std::string tip = tipOf(e);
			sl.button->tooltip += (sl.button->tooltip.empty() || tip.empty() ? "" : "\n") + tip;
			sl.items.push_back(e.item);
		}
		for (Slot& sl : slots_)
		{
			const bool used = !sl.items.empty();
			sl.button->enabled = used;
			if (!used)
			{
				sl.label->colour = res::colour("Main").alpha(0.35f);
				sl.label->text = "-";
			}
		}
		extra_->text = extra;
		fill(*pack_, packItems_);

		const InventoryContainer& c = s.container;
		window_->rect.w = c.valid ? kWide : kNarrow;
		containerCol_->visible = c.valid;
		containerTitle_->text = c.name.empty() ? "CONTAINER" : c.name;
		containerTitle_->colour = c.foreign ? res::colour("BadBright") : res::colour("Main").alpha(0.7f);
		foreignNote_->visible = c.foreign;
		fill(*container_, c.items);
	}
}
