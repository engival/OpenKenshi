#pragma once
// Control hints: one action list per hovered target, the list the cursor, the click dispatch, the hold menu and the hints panel are all views of
// (kenshi-k16.2.294.2, step 0 of the design in desktop-findings/kenshi-k16.2.294.1/DESIGN.md 2 and 5). Step 0 only carries the types: hints_for
// returns an empty list, nothing in the game reads it yet, and hover_cursor / sim_click / menu_items decide as they did. Each later step moves one
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
		int cursor = -1;	// the CursorId this action wants shown when it is the hover's default (RMB / the "primary" one); -1: none
		std::function<bool()> run;
		std::string log;
		bool menu = false;	// also a hold-RMB row (replaces menu_items, DESIGN 4 step 9)
		char key = 0;		// an optional key shown with it ("V")
	};

	// What is under the pointer: the target's name for the panel and the F12 sidecar, its actions (priority-ordered: the primary right click first),
	// and the "rule:" line the sidecar prints beside the cursor.
	struct Hover
	{
		std::string target;
		std::vector<Action> actions;
		const char* rule = "";
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
	};

	// The target's action list. Step 0: the target name only, no actions (each later step adds its kind).
	Hover hints_for(const Pick& p, const Context& ctx);
	// A HUD portrait (a squad bar slot or a cast row): its click, Shift+click and double click, the same list the HUD's closures run.
	Hover hints_for_portrait(uint64_t id, const Context& ctx);
	// The action a gesture dispatches, or null. Step 0: the gesture alone (the double-click pair and the modifier rules arrive with the first
	// resolver, DESIGN 2 "Dispatch").
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
