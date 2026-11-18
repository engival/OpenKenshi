#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// A character's inventory window (docs/UI.md): worn / wielded slots on the left, the pack as a list with counts beside them, Drop with a count,
// equip / unequip, and an open container (loot, storage) as a third column with Take / Put / Take all.
// Like the Hud it knows nothing about SIMLINK: the host fills an InventoryState from its sim view every frame and turns InventoryActions into orders.
// A plain list for now (the sim has no item sizes); a grid can replace the pack list without changing the state / actions.
namespace ui
{
	struct InventoryEntry
	{
		uint32_t item = 0;	// opaque to the UI: handed back in actions (the item strid)
		std::string name;	// display name; empty: "#item"
		uint32_t count = 0;
		int slot = 0;		// 0 in the pack, 1 wielded weapon, 2 + n worn in ARMOUR slot n
		bool equippable = false;	// a WEAPON or ARMOUR record: a pack entry offers Equip
		int grade = -1;		// 0..5 (ITEM_QUALITY), -1 none: the name is drawn in the grade's colour (ItemGrade0..4, ItemArtifact for 5)
		std::string quality = {};	// shown in the row's tooltip: a weapon's model name ("Catun No.1"), the grade word for armour
		bool operator==(const InventoryEntry&) const = default;
	};

	// An open container beside the character: its INVENTORY (every entry slot 0). The host clears it (valid false) when the window closes or
	// the character walks away.
	struct InventoryContainer
	{
		bool valid = false;
		uint64_t id = 0;
		std::string name;	// empty: "CONTAINER"
		std::vector<InventoryEntry> items;
		bool foreign = false;	// a theft event was seen for it: taking from it is theft (title in red)
		bool operator==(const InventoryContainer&) const = default;
	};

	struct InventoryState
	{
		bool valid = false;	// false: nothing to show yet (no character, or no INVENTORY received)
		uint64_t owner = 0;
		std::string ownerName;
		std::vector<InventoryEntry> items;
		InventoryContainer container;
		bool operator==(const InventoryState&) const = default;
	};

	struct InventoryActions
	{
		std::function<void(uint64_t owner, uint32_t item, uint32_t count)> drop;	// order 61 (count fits u16: at most 65535 is asked)
		std::function<void(uint64_t owner, uint32_t item, bool on)> equip;	// order 62: on = equip a pack entry, off = unequip a worn / wielded one
		std::function<void(uint64_t owner, uint64_t container, uint32_t item, uint32_t count, bool take)> transfer;	// order 63 (take: container -> pack)
		std::function<void(uint64_t owner, uint64_t container)> takeAll;	// every stack of the container into the pack (transfers, host's choice)
	};

	class Inventory
	{
	public:
		explicit Inventory(InventoryActions actions);
		~Inventory();
		void update(const InventoryState& s);	// every frame, before ui::frame (cheap when nothing changed)
		bool shown() const;
		void show(bool on);
		// For scripted tests; null when out of range. Double-click: a pack row equips (when equippable), a slot unequips, a container row takes.
		Widget* packRow() const;	// the pack list
		Widget* containerRow() const;	// the container list (hidden without a container)
		Widget* dropButton() const { return drop_; }
		Widget* equipButton() const { return equip_; }
		Widget* unequipButton() const { return unequip_; }
		Widget* putButton() const { return put_; }
		Widget* takeButton() const { return take_; }
		Widget* takeAllButton() const { return takeAll_; }
		Widget* countBox() const;
		Widget* slotButton(size_t i) const { return i < slots_.size() ? slots_[i].button : nullptr; }	// the equipment rows, in display order
		Widget* slotLabel(size_t i) const { return i < slots_.size() ? slots_[i].label : nullptr; }	// their item labels
		int selected() const { return side_ == Side::kPack ? sel_ : -1; }	// index into the pack, -1 none
		int selectedContainer() const { return side_ == Side::kContainer ? sel_ : -1; }	// index into the container
		int selectedSlot() const { return side_ == Side::kSlot ? sel_ : -1; }	// equipment row
		uint32_t dropCount() const;	// the count box, clamped to the selected stack (Drop, Put and Take use it)

	private:
		enum class Side { kNone, kPack, kContainer, kSlot };
		struct Slot { Button* button; Label* label; std::vector<uint32_t> items; };
		Window* window_ = nullptr;
		Label* title_ = nullptr;
		Label* empty_ = nullptr;
		Widget* body_ = nullptr;
		std::vector<Slot> slots_;
		Label* extra_ = nullptr;	// worn in slots this window has no row for
		List* pack_ = nullptr;
		Widget* containerCol_ = nullptr;
		Label* containerTitle_ = nullptr;
		Label* foreignNote_ = nullptr;
		List* container_ = nullptr;
		EditBox* count_ = nullptr;
		Button *drop_ = nullptr, *equip_ = nullptr, *unequip_ = nullptr, *put_ = nullptr, *take_ = nullptr, *takeAll_ = nullptr;
		std::vector<Widget*> countRow_;	// enabled with a pack or container selection
		InventoryActions act_;
		InventoryState last_;
		std::vector<InventoryEntry> packItems_;
		Side side_ = Side::kNone;
		int sel_ = -1;
		void rebuild();
		void select(Side side, int i);
		const std::vector<InventoryEntry>* listOf(Side side) const;	// pack or container entries
		const InventoryEntry* selectedEntry() const;
		void setCount(int64_t n);
		uint32_t maxCount() const;
		void doEquip();
		void doUnequip();
		void doTransfer(bool take);
	};
}
