#pragma once
// The pick under the pointer and the target predicates a hover / click / menu decision reads (kenshi-k16.2.294.2, step 0 of the control-hints
// design: desktop-findings/kenshi-k16.2.294.1/DESIGN.md 2, 4 and 7). Moved out of main.cpp unchanged, so pick.cpp can carry them beside
// controlhints.cpp:
//   Pick          what the mouse ray hits first (main.cpp's pick_at fills it; pick_screen is its screen entry)
//   CharAct       what a right click on a character does, and the cursor it wants
//   the helpers   sneak_actor / seen_by / locked_cage / container_locked / char_act: the sim predicates hover_cursor, sim_click and menu_items
//                 each re-ask (DESIGN 1 observation 1)
// The app state comes in as arguments (the sim; char_act also the drawn characters and the Ctrl modifier), so this file needs none of main.cpp's
// globals and no sokol. Behaviour neutral: pick.cpp is main.cpp's old code, line for line.
#include <cstdint>

#include "net/simlink.hpp"

class Characters;
class SimView;

// What the mouse ray hits first (SIMLINK 13, CURSORS_LIVE: the nearest hit wins, no layer priority): a character, a ground item, a door, or the world
// (terrain and collision shapes; hit holds its point). A character or item counts as nearer within its pick reach past the world hit, so a click at
// its feet still takes it. A door is its leaf's own collision, at the open pose while open (kinspect findings/door_pick.md: an open gate's opening
// is clear, only the raised leaf picks), hit before the world; kDoorMatch: MINE, the server's door position is the centre of its navmesh polygons.
// kenshi-k16.2.259: the ray starts at the near clip plane (camera_near) and EVERY layer passes what is not drawn there (pick_hidden), so the click takes
// the first drawn surface along it; hit is the picked thing's own hit point (not the ground ray's, the F12 report's old fault).
struct Pick
{
	enum Kind { None, Ground, Character, Item, Door, Container, Producer, Bed } kind = None;
	uint64_t id = 0;
	double hit[3] = {};
	bool building = false;	// Ground: the hit is a building's shape (the original's hover candidate: no invalidmove there)
	uint64_t scenery = 0;	// Ground: the anonymous foliage / shape hit's identity (a hash of its world box), 0 = none: debug.pick_mask's subject for scenery with no sim id (kenshi-l5dx)
	float sceneryBox[6] = {};	// its world box
};

struct CharAct { const char* cursor = "arrow"; int action = 0; };
constexpr int kAttack = -1, kFollow = -2;

// The sneak actor: the first selected character, in selection order (the order the server receives in the order 60 frame), that sneaks and carries
// nobody; 0 when none does. With several selected this is the one char_act and menu_items use, both for the sneak test and for the SEEN_BY set: the
// server's KO chooses its own assassin among the selection's sneakers and SEEN_BY is per character, so one actor is as close as the client can get.
uint64_t sneak_actor(const SimView& sim);
// Whether character observer knows sneaking character id (SEEN_BY 59, the stealth KO's own abort test): absent from the set (or no frame yet) = no.
bool seen_by(const SimView& sim, uint64_t id, uint64_t observer);
// A locked cage's prisoner (CAGES, kenshi-k16.2.105.1): no default click, the padlock cursor, two menus. null: not caged, or its cage is open.
const simlink::SimLinkDecoder::Cage* locked_cage(const SimView& sim, uint64_t id);
// A locked box (kind 3 state 2, SIMLINK's container lock): the click acts as the pick (order 60 action 3) and the sim answers; the window and
// the loot wait for the unlock, as the original's cursor / task pair. The state is the wire's, never a client rule.
bool container_locked(const SimView& sim, uint64_t id);
// What a right click on character id does, and its cursor (the exe's character handler 1407fb410; kinspect findings/cursors.md rows C1-C22):
// ours dead: search, loot (C9); ours hurt: med, first aid (C10); ours down and not carried, someone else selected: lift, carry (C13); ours carried
// by the selection: lift, put down (C14); anyone else down or dead: search, loot (C2, C22: scavenge vs search is UNSURE there, search here);
// anyone else alive: attack, or knockout (cursor 0x19, order 60 action 14 stealth KO) while a selected character sneaks and carries nobody and the
// hostile has NOT seen it (absent from its SEEN_BY set, SIMLINK 59: the KO's own abort test; the set holds every non-allied observer, hostility is
// tested separately by kHostile); a hostile that has seen it is attacked (C7, C21; the
// exe's two-portrait cursor pair is not drawn). MINE: ours otherwise is green (follow) while someone else is selected (C12 has no cursor; talk
// and cages wait on the sim). action: order 60's action, or kAttack / kFollow; 0: nothing. ctrl: the Ctrl key held (forces the attack, MINE: the
// original's is the hold menu's Attack).
CharAct char_act(const SimView& sim, const Characters& chars, uint64_t id, bool ctrl);
