#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The squads page of the overview window (docs/UI.md), after the original's SQUADS tab (kenshi-inspect findings/ui_captures/squads.md, squads13_static.md); layout
// ours. ADD SQUAD and the faction's "used/max" top right; a row per squad in the host's order: its name (an edit box: renamed on every
// keystroke), "members/max", DELETE, a strip of its members' portraits; the DISMISS drop zone bottom right. Everything is drag and drop:
// a portrait onto a strip moves the character (onto a portrait: to that place, 0 = the leader; Shift: swaps the two), onto DISMISS asks to
// dismiss them; a row onto another row reorders the squads. Portraits do nothing on hover or click. Refusals are the original's message boxes.
// The active squad is picked on the HUD (HudState::squadTabs). Like the Gear it knows nothing about SIMLINK: the host fills a SquadsState every
// frame and turns the actions into orders.
namespace ui
{
	class Prompt;
	class Overview;

	struct SquadMember
	{
		uint64_t id = 0;
		std::string name;	// empty: "#id"
		PortraitRef portrait = {};	// none: the frame shows the initial
		PortraitMark mark = PortraitMark::kNone;	// the portrait's layers, as HudMember's
		PortraitBack back = PortraitBack::kNone;
		bool dim = false;
		bool hurt = false;
		bool down = false;	// KO or dead: dimmed
		bool operator==(const SquadMember&) const = default;
	};

	struct SquadInfo
	{
		uint64_t id = 0;
		std::string name;	// may be empty
		std::vector<SquadMember> members;	// [0] = the leader
		int max = 20;	// "max squad size" (GlobalConstants); shown "members/max", red at the max, a drop past it refused (only swaps)
		bool operator==(const SquadInfo&) const = default;
	};

	struct SquadsState
	{
		bool valid = false;	// false: nothing to show yet
		std::vector<SquadInfo> squads;
		int used = 0;		// the faction's living members (all squads)
		int factionMax = 0;	// "max faction size" (GlobalConstants, 30): "used/factionMax" top right, red at the max; 0: not shown
		int maxSquads = 0;	// "max squads" (GlobalConstants, 10): ADD SQUAD at it says so instead; 0: no limit
		bool operator==(const SquadsState&) const = default;
	};

	// Each is one order for the host; the window changes nothing itself and shows the next state.
	struct SquadsActions
	{
		std::function<void()> newSquad;	// ADD SQUAD below maxSquads; the host names it ("<faction>_<N>")
		std::function<void(uint64_t squad, const std::string& name)> rename;	// every keystroke (and Escape's restore); "" allowed; 32 bytes at most
		std::function<void(uint64_t squad)> remove;	// DELETE on an empty squad that is not the only one
		// A portrait dropped on a strip: the member ends at index in that squad (others move along), -1 = the end. The same squad: a reorder.
		std::function<void(uint64_t member, uint64_t squad, int index)> moveMember;
		std::function<void(uint64_t a, uint64_t b)> swapMembers;	// Shift + a drop on another portrait (either squad): the two trade places
		std::function<void(uint64_t squad, int index)> moveSquad;	// a row dropped on another row: the squad ends at index (the HUD tabs follow)
		std::function<void(uint64_t member)> dismiss;	// dropped on DISMISS, after the prompt's Yes; never the faction's last character
		std::function<void()> close;	// the overview closed or left the page while it showed squads
	};

	class Squads
	{
	public:
		Squads(Overview& home, SquadsActions actions);	// its content goes on home's SQUADS page
		~Squads();
		void update(const SquadsState& s);	// every frame, before ui::frame (cheap when nothing changed)
		bool shown() const;	// the overview shows its page
		void show(bool on);	// on: the overview opens on it; off: the overview closes if it shows it
		// For scripted tests; null when there is no such squad / member.
		Widget* window() const;	// the overview's
		Widget* addButton() const;
		Widget* dismissZone() const;
		Widget* block(uint64_t squad) const;	// the squad's row (drag it to reorder)
		Widget* strip(uint64_t squad) const;	// its portraits (a drop target)
		Widget* nameBox(uint64_t squad) const;	// an EditBox
		Widget* count(uint64_t squad) const;	// the "members/max" Label
		Widget* used() const;	// the "used/factionMax" Label
		Widget* deleteButton(uint64_t squad) const;
		Widget* member(uint64_t id) const;	// the portrait
		Widget* memberMark(uint64_t id) const;	// the Image over its face (SquadMember::mark)
		const PortraitFace* memberFace(uint64_t id) const;	// its layers
		Prompt& prompt() const { return *prompt_; }

	private:
		class Cell;
		class Block;
		class Strip;
		class Zone;
		struct Row { uint64_t id; Block* block; EditBox* name; Label* count; Button* del; Strip* strip; std::vector<Cell*> cells; };
		SquadsActions act_;
		Overview& home_;
		bool wasShown_ = false;
		Label* empty_ = nullptr;
		Widget* body_ = nullptr;
		Button* add_ = nullptr;
		Zone* dismiss_ = nullptr;
		Label* used_ = nullptr;
		ScrollView* list_ = nullptr;
		std::vector<Row> rows_;
		std::unique_ptr<Prompt> prompt_;
		SquadsState last_;
		void rebuild();
		void refresh();	// values into the existing widgets
		void tell(const std::string& text);	// the original's untitled Ok box
		void dropMember(uint64_t member, float x, float y);	// a portrait let go at (x, y), pixels
		void dropSquad(uint64_t squad, float x, float y);	// a row let go
		void askDismiss(uint64_t member);
	};
}
