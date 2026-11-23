#pragma once
// Control hints: one action list per hovered target, the list the cursor, the click dispatch, the hold menu and the hints panel are all views of
// (kenshi-k16.2.294.2, step 0 of the design in desktop-findings/kenshi-k16.2.294.1/DESIGN.md 2 and 5). Step 0 only carries the types: hints_for
// returns an empty list, nothing in the game reads it yet, and hover_cursor / sim_click decide as they did. Each later step moves one
// target kind into hints_for and deletes its old branch.
//
// The gesture-name table lives here, host side, one string per Gesture (DESIGN 3: key rebinds and the "Shift+" prefix come from one place); the UI
// panel gets plain text lines across the kui boundary, never these types.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "input/pick.hpp"

class Characters;
class SimView;

namespace hints
{
	enum class Gesture : uint8_t
	{
		kClick, kShiftClick, kCtrlClick, kAltClick, kDoubleClick, kRightClick, kShiftRightClick, kCtrlRightClick, kAltRightClick, kDoubleRightClick,
		kHoldRight, kDrag, kKey
	};
	const char* gesture_name(Gesture g);	// "Click", "Shift+Click", "Right click", ... (the table; one per Gesture)

	// One thing the pointer's target can do. kind is the stable id ("move", "attack", "follow_order", "drive", "select", "pick_up", "lockpick", ...):
	// it is also the guide-coverage key (HINT_KIND below). label is the text the hints panel and the hold menu row both show. run is the code that
	// runs; its bool is "sent" (order_sent / the Right_Click sound). log is the printf line the click dispatch prints for it, kept equal to today's
	// text so scripts that grep the log pass unchanged.
	struct Action
	{
		Gesture gesture = Gesture::kClick;
		const char* kind = "";
		std::string label;
		bool enabled = true;
		std::string why;	// the greyed reason: "needs a first-aid kit", "locked: hold ALT" (empty when enabled)
		std::string cursor;	// the cursor name (Cursors::set) this action wants shown when it is the hover's default (the primary right click); empty: none
		std::function<bool()> run;
		std::string log;
		bool logSent = true;	// the click dispatch appends ": sent" / ": not sent" to log (what today's order lines print); false: log is the whole line
		std::function<std::string(bool)> after;	// the whole line when it needs the state after run (a select's selection count); wins over log
		bool menu = false;	// also a hold-RMB row (the hold menu's rows, DESIGN 4 step 9)
		char key = 0;		// an optional key shown with it ("V")
		int percent = -1;	// a chance the row's label carries ("Pick Lock 62%"): the hold menu draws it as its own column; -1: none
		std::string menuText;	// the hold menu's row text when it differs from label (no percent suffix); empty: label
	};

	// What is under the pointer: the target's name for the panel and the F12 sidecar, its actions (priority-ordered: the primary right click first),
	// and the "rule:" line the sidecar prints beside the cursor.
	struct Hover
	{
		std::string target;
		std::vector<Action> actions;
		std::string rule;
		std::string menuTitle;	// the hold menu's title row (the target's name); empty: this target has no menu
	};

	// The state a resolution reads, filled by the host at the moment it resolves (DESIGN 2): the modifiers as held NOW (hover) or at the click
	// (dispatch), so the panel re-resolves when Shift / Ctrl / Alt change.
	struct Context
	{
		const SimView& sim;
		const Characters& chars;
		bool alt = false, ctrl = false, shift = false;
		bool paused = false;	// the sim's pause bit (pauseBits & 1)
		int floor = 0;			// the floor the player sees (App::buildings.level)
		bool following = false;	// the follow camera / direct drive is on, and on whom (App::follow / followId)
		uint64_t followId = 0;
		int entries = -1;		// MENU_ENTRIES for the hovered target; -1: none asked or not answered
		// Portraits (step 1): the selection, whether the RTS camera is centred on a character (and which), and the host's own calls the portrait
		// actions run, so the label and the behaviour come from the same functions (squad_leader, portrait_click, set_rts).
		std::vector<uint64_t> selection;
		bool centred = false;	// the RTS camera auto-centres on centerId (Z / a portrait double click), not direct drive
		uint64_t centerId = 0;
		std::function<uint64_t(const std::vector<uint64_t>&)> squadLeader;	// the first of the list in squad order (main.cpp squad_leader)
		std::function<void(uint64_t, bool)> select;	// a portrait click (add: Shift)
		std::function<void(uint64_t)> focus;		// a portrait double click: a player's character selects and centres, anyone else's is the watch
		// Characters in the world (steps 2 and 3): the host's own calls the actions run, so the click and the label share one code path.
		std::function<void(uint64_t, bool)> worldSelect;	// a left click on a character: select it (own: the panel goes with it, Shift adds) or inspect it (anyone else's)
		std::function<bool(uint64_t, int, bool)> charOrder;	// char_act's action on id (pick.hpp kAttack / kFollow / order 60's number), queued when the bool is set
		std::function<bool(uint16_t, uint64_t)> charJob;	// a Shift job (order 76): the job number and its target
		std::function<bool(uint64_t)> drive;		// direct drive on an own character: select it and follow it
		std::function<bool()> leaveDrive;			// leave the direct drive
		// Things in the world (steps 4 and 5): items, beds, producers, doors. The host's own calls the rows run (the old sim_click's), and the pointer's state.
		uint64_t labelId = 0;		// the ground item or container whose label is under the pointer (App label_at); 0: none
		std::function<bool(uint64_t, int, bool)> thingOrder;	// order 60 on a thing: SimView::interact(id, action, queue) (7 pick up, 9 sleep, 1 open, 2 close)
		std::function<void(uint64_t)> showMachine;				// a left click on a producer: its machine window, the selection kept
		std::function<bool(uint64_t, bool)> producerOpen;		// an automatic producer's right click: open it (walk + window)
		std::function<bool(uint64_t, bool)> producerUse;		// a manned producer's right click: use (mine / operate), the panel follows it
		std::function<bool(uint64_t)> producerAutomatic;		// the machine needs no operators (a well, a furnace)
		std::function<std::string(uint64_t)> producerCursor;	// "mine" or "use"
		// Containers (step 6): the old sim_click's own calls
		std::function<bool(uint64_t, bool)> containerOpen;	// walk there and open its window (open_container)
		std::function<void(uint64_t)> containerShow;		// a left click: its window alone, the selection kept
		std::function<bool(uint64_t, bool)> foreignWalk;	// a right click on someone else's container without ALT: the HUD hint, and (orders.foreign_walk_to) the walk there
		bool foreignWalking = true;							// orders.foreign_walk_to
		// A locked cage's prisoner (step 7): the cage's MENU_ENTRIES answer and pick chance (-1: not answered), and the cage's own orders (18 pick lock, 19 tools, 20 strength)
		struct CageInfo { int entries = -1; float chance = -1; };
		std::function<CageInfo(uint64_t)> cageInfo;
		std::function<bool(uint64_t, int)> cageOrder;
		// Ground and buildings (step 8): the nav's answer at a point (0 no, 1 yes, 2 unknown: ClientNav::Reach) and the click's own move (sim_click's: the drawn hit snapped to
		// the navmesh, the sim's moveTo, the route marker; the string is the click's whole log line, filled in by the call)
		std::function<int(const double*)> walkable;
		std::function<bool(bool, std::string&)> groundMove;	// (queue, log): set only by the click dispatch, which owns the pick and the click record
		std::function<void(bool)> clearSelect;				// a left click on the ground: SimView::select(0, shift)
	};

	// The target's action list. Step 0: the target name only, no actions (each later step adds its kind).
	Hover hints_for(const Pick& p, const Context& ctx);
	// A HUD portrait (a squad bar slot or a cast row): its click, Shift+click and double click, the same list the HUD's closures run.
	Hover hints_for_portrait(uint64_t id, const Context& ctx);
	// A left click on nothing (the ground, the sky, a bed or a door that has no left click of its own): clears the selection (Shift keeps it).
	Action deselect_action(const Context& ctx);
	// The action a gesture dispatches, or null. Step 0: the gesture alone (the double-click pair and the modifier rules arrive with the first
	// resolver, DESIGN 2 "Dispatch").
	// A Shift or Ctrl variant with no row of its own falls back to the plain one (the order's own code reads ctx.shift / ctx.ctrl, as the click did).
	const Action* match(const Hover& h, Gesture g, const Context& ctx);
	// The list as text (the F12 sidecar's `hints:` block and the dev `hints at X Y` op share it): one line for the target, one per action.
	std::string format_hover(const Hover& h);

	// The kind registry (DESIGN 5): every Action.kind is registered once, at the resolver that can emit it, with the phrase docs/PLAYER_GUIDE.md
	// uses for it. The guide-coverage check (`hints kinds`, tools/guide_hints_check.sh) greps the guide for each phrase.
	struct HintKind { const char* id; const char* phrase; };
	const std::vector<HintKind>& hint_kinds();
	void register_hint_kind(const char* id, const char* phrase);	// a duplicate id says so on stderr and keeps the first (one home per kind)
}

// HINT_KIND("pick_up", "Pick up"); at namespace scope in the resolver's file: registers the pair once, before main.
#define HINT_KIND_JOIN2(a, b) a##b
#define HINT_KIND_JOIN(a, b) HINT_KIND_JOIN2(a, b)
#define HINT_KIND(id, phrase) [[maybe_unused]] static const bool HINT_KIND_JOIN(kHintKind_, __LINE__) = (hints::register_hint_kind(id, phrase), true)
