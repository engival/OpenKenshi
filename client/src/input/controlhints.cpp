// controlhints.hpp: the gesture names, the kind registry and the (empty) resolver (kenshi-k16.2.294.2, control-hints step 0). Read the header first.
#include "input/controlhints.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "characters/characters.hpp"
#include "bridge/simview.hpp"

namespace hints
{
const char* gesture_name(Gesture g)
{
	// The one table (DESIGN 3): the panel, the hold menu and the F12 sidecar word a gesture from here; no "Shift+" typed beside a branch.
	switch (g)
	{
		case Gesture::kClick: return "Click";
		case Gesture::kShiftClick: return "Shift+Click";
		case Gesture::kCtrlClick: return "Ctrl+Click";
		case Gesture::kAltClick: return "Alt+Click";
		case Gesture::kDoubleClick: return "Double click";
		case Gesture::kRightClick: return "Right click";
		case Gesture::kShiftRightClick: return "Shift+Right click";
		case Gesture::kCtrlRightClick: return "Ctrl+Right click";
		case Gesture::kAltRightClick: return "Alt+Right click";
		case Gesture::kDoubleRightClick: return "Double right click";
		case Gesture::kHoldRight: return "Hold right";
		case Gesture::kDrag: return "Drag";
		case Gesture::kKey: return "Key";
	}
	return "?";
}

namespace
{
// The kind registry's storage: a function-local static, so a HINT_KIND at another file's static-init time finds it already built.
std::vector<HintKind>& hint_registry()
{
	static std::vector<HintKind> kinds;
	return kinds;
}

const char* pick_kind_word(Pick::Kind kind)
{
	switch (kind)
	{
		case Pick::Character: return "character";
		case Pick::Item: return "item";
		case Pick::Door: return "door";
		case Pick::Container: return "container";
		case Pick::Producer: return "producer";
		case Pick::Bed: return "bed";
		default: return "";
	}
}

// The target's name for the panel and the sidecar. Step 0's wording only; each later step gives its target the titles DESIGN 0 lists
// ("Raider (hostile)", "Door (locked)", "Lockpicker (yours)") as it moves that kind into the resolver.
std::string target_name(const Pick& p, const Context& ctx)
{
	const auto quoted = [&](const char* kind, const std::string& name, uint64_t id)
	{
		const std::string n = name.empty() ? "#" + std::to_string(id) : name;
		return std::string(kind) + " \"" + n + "\"";
	};
	if (p.kind == Pick::Character)
	{
		const auto ch = ctx.sim.decoder().chars.find(p.id);
		const std::string name = ch != ctx.sim.decoder().chars.end() ? ctx.sim.decoder().str(ch->second.look.name) : "";
		return (name.empty() ? "#" + std::to_string(p.id) : name) + (ctx.sim.isPlayer(p.id) ? " (yours)" : " (not yours)");
	}
	if (p.kind == Pick::Ground)
	{
		if (p.building && p.id)	// a player's building: named off its BUILDING record
		{
			const simlink::SimLinkDecoder::Building* pb = ctx.sim.building(p.id);
			return quoted("building", pb ? ctx.sim.nameOf(pb->record, ctx.chars) : "", p.id);
		}
		return p.building ? "Ground (a building's shape)" : "Ground";
	}
	if (p.kind == Pick::None) { return "nothing"; }
	if (const simlink::Interactable* t = ctx.sim.thing(p.id)) { return quoted(pick_kind_word(p.kind), ctx.sim.nameOf(t->record, ctx.chars), p.id); }
	return quoted(pick_kind_word(p.kind), "", p.id);
}
}

HINT_KIND("select", "Click a portrait");
HINT_KIND("select_add", "Shift+click on a portrait");
HINT_KIND("centre", "double click on the character or its portrait");
HINT_KIND("watch", "double click on a cast row");

namespace
{
std::string char_name(uint64_t id, const Context& ctx)
{
	const auto ch = ctx.sim.decoder().chars.find(id);
	const std::string name = ch != ctx.sim.decoder().chars.end() ? ctx.sim.decoder().str(ch->second.look.name) : "";
	return name.empty() ? "#" + std::to_string(id) : name;
}
}


// The carry layer (kenshi-01b4, rwh.311): a character carries someone when a body's carriedBy names it (CHAR_DYNAMIC) or its carry flag bits are set.
static bool carries(const SimView& sim, uint64_t id)
{
	for (const auto& [cid, cc] : sim.decoder().chars) { if (cc.cur.carriedBy == id) { return true; } }
	const auto ch = sim.decoder().chars.find(id);
	return ch != sim.decoder().chars.end() && (ch->second.cur.flags & (simlink::kCarryL | simlink::kCarryR));
}
// A selected character carries someone: the exe's per-character order loop (1407f9e20) turns every order on a built bed into put-in-bed (99) and on a cage into
// put-in-cage (0x6c), whatever the menu row; the gear cursor stays.
static bool selection_carries(const SimView& sim)
{
	const auto& sel = sim.selected();
	return std::any_of(sel.begin(), sel.end(), [&](uint64_t s) { return carries(sim, s); });
}

// char_act (moved here from pick.cpp, kenshi-86qh: the character resolver's inner helper; the click, the cursor and the hint list all read it).
// What a right click on character id does, and its cursor (the exe's character handler 1407fb410; kinspect findings/cursors.md rows C1-C22):
// ours dead: search, loot (C9); ours hurt: med, first aid (C10); ours down and not carried, someone else selected: lift, carry (C13); ours carried
// by the selection: lift, put down (C14); anyone else down or dead: search, loot (C2, C22: scavenge vs search is UNSURE there, search here);
// anyone else alive: attack, or knockout (cursor 0x19, order 60 action 14 stealth KO) while a selected character sneaks and carries nobody and the
// hostile has NOT seen it (absent from its SEEN_BY set, SIMLINK 59: the KO's own abort test; the set holds every non-allied observer, hostility is
// tested separately by kHostile); a hostile that has seen it is attacked (C7, C21; the
// exe's two-portrait cursor pair is not drawn). MINE: ours otherwise is green (follow) while someone else is selected (C12 has no cursor; talk
// and cages wait on the sim). action: order 60's action, or kAttack / kFollow; 0: nothing. ctrl: the Ctrl key held (forces the attack, MINE: the
// original's is the hold menu's Attack).
CharAct char_act(const SimView& sim, const Characters& chars, uint64_t id, bool ctrl)
{
	const auto ch = sim.decoder().chars.find(id);
	if (ch == sim.decoder().chars.end()) { return {}; }
	const uint32_t flags = ch->second.cur.flags;
	const auto& sel = sim.selected();
	// a carrier is never the executor of a lift (rwh.311 Q1: 1407fae70 noCarry), so it does not count as "someone else"
	const bool other = std::any_of(sel.begin(), sel.end(), [&](uint64_t s) { return s != id && !carries(sim, s); });
	// someone else standing: attack a hostile (CHAR_DYNAMIC kHostile, the sim's own fight test), talk to anyone else (order 60 action 6; a trader
	// opens TRADE); Ctrl forces the attack (MINE: the original's is the hold menu's Attack)
	const uint64_t actor = sneak_actor(sim);
	if (!sim.isPlayer(id))
	{
		if (flags & (simlink::kKO | simlink::kDead)) { return { "search", 5 }; }
		// sneaking: a hostile that has not seen the actor is the knock-out (order 60 action 14, the red fist); one that has seen it is attacked as
		// without sneak; a non-hostile is used as without sneak (SIMLINK SEEN_BY client rule, user 2026-11-11; Ctrl still forces the attack)
		if (actor && !ctrl && (flags & simlink::kHostile)) { return seen_by(sim, actor, id) ? CharAct{ "attk", kAttack } : CharAct{ "knockout", 14 }; }
		return ctrl || (flags & simlink::kHostile) ? CharAct{ "attk", kAttack } : CharAct{ "talk", 6 };
	}
	if (flags & simlink::kDead) { return { "search", 5 }; }
	if (sim.health(id, chars) < 1.0f) { return { "med", 10 }; }
	if (flags & simlink::kKO)
	{
		const uint64_t by = ch->second.cur.carriedBy;
		if (!by) { return other ? CharAct{ "lift", 8 } : CharAct{}; }
		return std::find(sel.begin(), sel.end(), by) != sel.end() ? CharAct{ "lift", 13 } : CharAct{};
	}
	return other ? CharAct{ "green", kFollow } : CharAct{};
}

HINT_KIND("char_select", "a character selects it");
HINT_KIND("char_add", "Shift adds it to the selection");
HINT_KIND("inspect", "inspects it in the status and medical panels");
HINT_KIND("drive", "double right click on one of your own characters");
HINT_KIND("leave_drive", "a double right click on the character being driven");
HINT_KIND("follow_order", "with someone else selected too it is the *follow* order");
HINT_KIND("stay_close", "Shift makes attack, follow and first aid into jobs");
HINT_KIND("first_aid", "first aid on a hurt character of yours");
HINT_KIND("medic_job", "Medic job");
HINT_KIND("lift", "lift a knocked-out character of yours");
HINT_KIND("put_down", "put a carried character down");
HINT_KIND("loot", "loot a body");
HINT_KIND("attack", "a character is attacked");
HINT_KIND("fight_job", "Fight enemies job");
HINT_KIND("talk", "talked to");
HINT_KIND("knock_out", "is the knock-out (**red fist**)");
HINT_KIND("caged", "a right click orders nothing");
HINT_KIND("cage_pick_lock", "the chance of picking the lock");
HINT_KIND("cage_tools", "Use Tools");
HINT_KIND("cage_strength", "Use Strength");

namespace
{
void character_menu(Hover& h, uint64_t id, const Context& ctx);
void thing_menu(Hover& h, uint64_t id, const Context& ctx);
const char* char_kind(const CharAct& a, bool mine)
{
	if (a.action == kAttack) { return "attack"; }
	if (a.action == kFollow) { return "follow_order"; }
	switch (a.action)
	{
		case 5: return "loot";
		case 6: return "talk";
		case 8: return "lift";
		case 10: return "first_aid";
		case 13: return "put_down";
		case 14: return "knock_out";
	}
	(void)mine;
	return "";
}

std::string char_verb(const CharAct& a, const std::string& n)
{
	switch (a.action)
	{
		case kAttack: return "Attack " + n;
		case kFollow: return "Follow " + n;
		case 5: return "Loot " + n;
		case 6: return "Talk to " + n;
		case 8: return "Pick up " + n + " (carry)";
		case 10: return "First aid on " + n;
		case 13: return "Put " + n + " down";
		case 14: return "Knock " + n + " out (stealth)";
	}
	return "";
}

std::string ull(uint64_t v) { return std::to_string(v); }

// A character in the world (steps 2 and 3 of DESIGN 4): own and anyone else's, one resolver. The click on it, its cursor and the F12 / `hints at` list
// all read these rows. A locked cage's prisoner gets only its left click: the padlock cursor and the menus are step 7's, still the old branches.
void character_hints(Hover& h, const Pick& p, const Context& ctx)
{
	const uint64_t id = p.id;
	const std::string n = char_name(id, ctx);
	const bool mine = ctx.sim.isPlayer(id);
	const std::vector<uint64_t>& sel = ctx.sim.selected();
	const bool selected = std::find(sel.begin(), sel.end(), id) != sel.end();
	const SimView& sim = ctx.sim;
	if (mine)
	{
		Action sel1;
		sel1.gesture = Gesture::kClick;
		sel1.kind = "char_select";
		sel1.label = "Select " + n;
		sel1.logSent = false;
		sel1.run = [ws = ctx.worldSelect, id] { if (ws) { ws(id, false); } return true; };
		sel1.after = [&sim, id](bool) { return "character " + ull(id) + ", " + std::to_string(sim.selected().size()) + " selected"; };
		h.actions.push_back(sel1);
		Action add = sel1;	// SimView::select(id, true) only ever adds: a selected one shows greyed (kenshi-86qh: kept; the oracle's answer on kenshi-9p23 may change it)
		add.gesture = Gesture::kShiftClick;
		add.kind = "char_add";
		add.label = "Add " + n + " to the selection";
		if (selected) { add.enabled = false; add.why = "already selected"; }
		add.run = [ws = ctx.worldSelect, id] { if (ws) { ws(id, true); } return true; };
		h.actions.push_back(add);
	}
	else
	{
		Action in;
		in.gesture = Gesture::kClick;
		in.kind = "inspect";
		in.label = "Inspect " + n + " (the selection stays)";
		in.logSent = false;
		in.log = "inspect " + ull(id);
		in.run = [ws = ctx.worldSelect, id] { if (ws) { ws(id, false); } return true; };
		h.actions.push_back(in);
	}
	character_menu(h, id, ctx);
	if (const simlink::SimLinkDecoder::Cage* cg = locked_cage(sim, id))
	{
		// A caged prisoner has no default order: the padlock cursor, and the two-column menu opened on the press is the choice (kenshi-k16.2.105.1). The cage's
		// rows come from its MENU_ENTRIES answer (bits 16 pick lock, 17 tools, 18 strength; unanswered: all) and the pick chance (order 99 on the cage key).
		Action no;
		no.gesture = Gesture::kRightClick;
		no.kind = "caged";
		no.label = "Caged: hold right click for Pick Lock, Use Tools or Use Strength";
		no.enabled = false;
		no.why = "the menu opens on the press";
		no.cursor = "lock";
		no.logSent = false;
		no.log = "caged character " + ull(id) + ": no default order";
		no.run = [] { return false; };
		h.actions.push_back(no);
		const Context::CageInfo ci = ctx.cageInfo ? ctx.cageInfo(cg->cage) : Context::CageInfo{};
		struct CageRow { int bit; int action; const char* kind; const char* text; const char* log; };
		static const CageRow kRows[] = { { 16, 18, "cage_pick_lock", "Pick Lock", "pick lock" }, { 17, 19, "cage_tools", "Use Tools", "use tools" }, { 18, 20, "cage_strength", "Use Strength", "use strength" } };
		for (const CageRow& r : kRows)
		{
			if (ci.entries >= 0 && !(ci.entries & (1 << r.bit))) { continue; }
			Action m;
			m.gesture = Gesture::kHoldRight;
			m.kind = r.kind;
			m.menu = true;
			m.menuText = r.text;
			m.label = r.text;
			if (r.action == 18 && ci.chance >= 0)	// the original's int(chance x 1000) x 0.1
			{
				m.percent = (int)(std::floor(ci.chance * 1000.0f) * 0.1f);
				m.label += " " + std::to_string(m.percent) + "%";
			}
			m.log = std::string("cage ") + r.log + " " + ull(cg->cage);
			m.run = [co = ctx.cageOrder, cage = cg->cage, a = r.action] { return co && co(cage, a); };
			h.actions.push_back(m);
		}
		h.target += " (caged)";
		h.rule = "character in a locked cage -> lock (padlock, pick chance; a click orders nothing)";
		return;
	}

	// The right click: char_act's choice, then the Shift variant (the original's jobs, order 76) and the rows only a modifier reaches.
	const CharAct a = char_act(sim, ctx.chars, id, ctx.ctrl);
	const std::string line = "character " + ull(id) + " " + a.cursor + " (" + std::to_string(a.action) + ")";
	if (a.action != 0)
	{
		Action rc;
		rc.gesture = Gesture::kRightClick;
		rc.kind = char_kind(a, mine);
		rc.label = char_verb(a, n);
		if (a.action == kAttack && ctx.ctrl) { rc.label += " (Ctrl forces it)"; }
		rc.cursor = a.cursor;
		rc.log = line;
		rc.run = [co = ctx.charOrder, id, act = a.action, queue = ctx.shift] { return co && co(id, act, queue); };
		h.actions.push_back(rc);
		const uint16_t job = a.action == kAttack ? 4 : a.action == kFollow ? 31 : a.action == 10 ? 25 : 0;
		if (job)
		{
			Action sj = rc;
			sj.gesture = Gesture::kShiftRightClick;
			sj.kind = job == 4 ? "fight_job" : job == 31 ? "stay_close" : "medic_job";
			sj.label = job == 4 ? "Fight enemies (job)" : job == 31 ? "Stay close to " + n + " (job)" : "Medic job on " + n;
			sj.log = line + " as a job";
			sj.run = [cj = ctx.charJob, job, id] { return cj && cj(job, job == 31 ? id : 0); };
			h.actions.push_back(sj);
		}
	}
	if (!mine && !ctx.ctrl && a.action != kAttack && !(sim.decoder().chars.count(id) && (sim.decoder().chars.at(id).cur.flags & (simlink::kKO | simlink::kDead))))
	{
		Action f;	// Ctrl forces the attack on anyone alive (MINE: the original's is the hold menu's Attack)
		f.gesture = Gesture::kCtrlRightClick;
		f.kind = "attack";
		f.label = "Attack " + n;
		f.cursor = "attk";
		f.log = "character " + ull(id) + " attk (" + std::to_string(kAttack) + ")";
		f.run = [co = ctx.charOrder, id, queue = ctx.shift] { return co && co(id, kAttack, queue); };
		h.actions.push_back(f);
	}
	if (mine)
	{
		Action d;	// two right clicks within kDoubleClick on one's own character: direct drive on it (select + V); on the driven one: leave (k16.2.238.1)
		d.gesture = Gesture::kDoubleRightClick;
		d.logSent = false;
		if (ctx.following && ctx.followId == id)
		{
			d.kind = "leave_drive";
			d.label = "Leave direct drive on " + n;
			d.log = "double right click, character " + ull(id) + ": leave the drive";
			d.run = [ld = ctx.leaveDrive] { return ld && ld(); };
		}
		else
		{
			d.kind = "drive";
			d.label = "Direct drive on " + n;
			d.log = "double right click, character " + ull(id) + ": direct drive on";
			d.run = [dr = ctx.drive, id] { return dr && dr(id); };
		}
		h.actions.push_back(d);
	}
	h.rule = a.action ? "character -> char_act: " + std::string(a.cursor) : "character -> char_act: no order (" + std::string(a.cursor) + ")";
}
}

Hover hints_for_portrait(uint64_t id, const Context& ctx)
{
	Hover h;
	const std::string n = char_name(id, ctx);
	const bool mine = ctx.sim.isPlayer(id);
	h.target = n + (mine ? " (portrait)" : " (cast row)");
	const bool driving = ctx.following;	// direct drive: portrait clicks leave the follow target alone
	const bool follows = ctx.centred && !driving && ctx.centerId;
	if (!mine)
	{
		Action w;
		w.gesture = Gesture::kDoubleClick;
		w.kind = "watch";
		w.label = "Watch " + n + " (inspect and follow)";
		w.log = "portrait: watch " + std::to_string(id);
		w.run = [focus = ctx.focus, id] { if (focus) { focus(id); } return true; };
		h.actions.push_back(w);
		h.rule = "portrait of a character that is not yours: double click watches it";
		return h;
	}
	const std::vector<uint64_t>& sel = ctx.selection;
	const bool in = std::find(sel.begin(), sel.end(), id) != sel.end();
	Action click;
	click.gesture = Gesture::kClick;
	click.kind = "select";
	click.label = "Select " + n;
	if (follows) { click.label += " (the camera follows " + n + ")"; }
	else if (driving) { click.label += " (direct drive stays on " + char_name(ctx.followId, ctx) + ")"; }
	click.log = "portrait: select " + std::to_string(id);
	click.run = [select = ctx.select, id] { if (select) { select(id, false); } return true; };
	h.actions.push_back(click);

	Action add;
	add.gesture = Gesture::kShiftClick;
	add.kind = "select_add";	// SimView::select(id, true) only ever adds: a selected character stays (no remove)
	add.label = "Add " + n + " to the selection";
	if (in) { add.enabled = false; add.why = "already selected"; }
	else if (follows && ctx.squadLeader)
	{
		std::vector<uint64_t> after = sel;
		after.push_back(id);
		if (const uint64_t lead = ctx.squadLeader(after)) { add.label += " (the camera follows " + char_name(lead, ctx) + ", first in squad order)"; }
	}
	add.log = "portrait: add " + std::to_string(id);
	add.run = [select = ctx.select, id] { if (select) { select(id, true); } return true; };
	h.actions.push_back(add);

	Action dbl;
	dbl.gesture = Gesture::kDoubleClick;
	dbl.kind = "centre";
	dbl.label = "Select " + n + " and centre the camera on it" + (driving ? " (ends direct drive)" : "");
	dbl.log = "portrait: centre " + std::to_string(id);
	dbl.run = [focus = ctx.focus, id] { if (focus) { focus(id); } return true; };
	h.actions.push_back(dbl);
	h.rule = "own portrait: click selects, Shift+click adds or removes, double click selects and centres";
	return h;
}

HINT_KIND("pick_up_label", "Left clicking an item label picks the item up");
HINT_KIND("pick_up", "an item is picked up");
HINT_KIND("pick_up_queue", "queues the rest");
HINT_KIND("sleep", "a bed is slept in");
HINT_KIND("sleep_queue", "queues the rest");
HINT_KIND("put_in_bed", "a carrier's order on a bed puts the carried one in it");
HINT_KIND("put_in_bed_queue", "queues the rest");
HINT_KIND("machine", "a production building's label opens its window");
HINT_KIND("use_producer", "a mine or deposit sends the selection to mine it");
HINT_KIND("open_automatic", "a machine that needs no operators");
HINT_KIND("man_turret", "a placed turret: the original mans it");
HINT_KIND("auto_machine", "an automatic converter or power building: gears, no order");
HINT_KIND("producer_queue", "queues the rest");
HINT_KIND("door_open", "a door opens or closes");
HINT_KIND("door_close", "a door opens or closes");
HINT_KIND("door_queue", "queues the rest");
HINT_KIND("door_locked", "a locked door is not opened by a right click");
HINT_KIND("door_broken", "a broken door has nothing to open");
HINT_KIND("open_label", "a container's label opens it");
HINT_KIND("lockpick_label", "a locked container's label picks the lock");
HINT_KIND("show_container", "a left click on a container opens its window alone");
HINT_KIND("open_container", "a container opens");
HINT_KIND("foreign_walk", "walks you there and says to hold `Left Alt`");
HINT_KIND("lockpick", "`Alt` + right click does it");
HINT_KIND("foreign_walk_queue", "queues the rest");
HINT_KIND("open_container_queue", "queues the rest");
HINT_KIND("lockpick_queue", "queues the rest");

namespace
{
// One order row and its Shift (queue) variant: the right click runs it now, Shift+right click queues it after the current orders. The row's own code takes
// the queue flag, so the click needs no modifier test of its own (a row with no Shift variant of its own would run with the click's Shift).
void order_rows(Hover& h, const char* kind, const char* queueKind, const std::string& label, const std::string& cursor, const std::string& log,
	const std::function<bool(bool)>& run, bool enabled = true, const std::string& why = "", const std::string& queuedLog = "")
{
	Action rc;
	rc.gesture = Gesture::kRightClick;
	rc.kind = kind;
	rc.label = label;
	rc.cursor = cursor;
	rc.log = log;
	rc.enabled = enabled;
	rc.why = why;
	rc.run = [run] { return run(false); };
	h.actions.push_back(rc);
	if (!enabled) { return; }
	Action sq = rc;
	sq.gesture = Gesture::kShiftRightClick;
	sq.kind = queueKind;
	sq.label = label + " (queued after the current orders)";
	sq.log = queuedLog.empty() ? log + " (queued)" : queuedLog;
	sq.run = [run] { return run(true); };
	h.actions.push_back(sq);
}

// Items, beds, producers and doors (steps 4 and 5 of DESIGN 4): the click on each, its cursor and the F12 / `hints at` list all read these rows.
void thing_hints(Hover& h, const Pick& p, const Context& ctx)
{
	const simlink::Interactable* t = ctx.sim.thing(p.id);
	const uint64_t id = p.id;
	const std::string n = t ? ctx.sim.nameOf(t->record, ctx.chars) : "";
	const std::string name = n.empty() ? "#" + ull(id) : n;
	thing_menu(h, id, ctx);
	const auto order = [to = ctx.thingOrder, id](int action) { return std::function<bool(bool)>([to, id, action](bool queue) { return to && to(id, action, queue); }); };
	if (p.kind == Pick::Item)
	{
		if (ctx.labelId == id)	// the label's own click: the item's name drawn over it (the pick under a label never takes the click)
		{
			Action lc;
			lc.gesture = Gesture::kClick;
			lc.kind = "pick_up_label";
			lc.label = "Pick up " + name;
			lc.log = "label: pick up " + ull(id) + (ctx.shift ? " (queued)" : "");
			lc.run = [to = ctx.thingOrder, id, q = ctx.shift] { return to && to(id, 7, q); };
			h.actions.push_back(lc);
		}
		order_rows(h, "pick_up", "pick_up_queue", "Pick up " + name, "pick_item", "pick up " + ull(id), order(7));
		h.rule = "ground item -> pick_item";
		return;
	}
	if (p.kind == Pick::Bed)
	{
		const bool occupied = t && t->version == 2;
		if (selection_carries(ctx.sim))	// the carry layer: the same wire order (60 action 9 while carrying is the sim's put-in, k16.6.216), the exe's label
		{
			order_rows(h, "put_in_bed", "put_in_bed_queue", "Put in " + name + (occupied ? " (occupied)" : ""), "use", "put in bed " + ull(id), order(9));
			h.rule = "bed, a selected character carries -> put in bed (99), cursor use (rwh.311: the exe's carry swap)";
			return;
		}
		order_rows(h, "sleep", "sleep_queue", "Sleep in " + name + (occupied ? " (occupied)" : ""), "use", "sleep in bed " + ull(id), order(9));
		h.rule = "bed -> use (no sleep cursor in CURSORS.md: the building fallback)";
		return;
	}
	if (p.kind == Pick::Producer)
	{
		Action ml;
		ml.gesture = Gesture::kClick;
		ml.kind = "machine";
		ml.label = "Show the machine window of " + name;
		ml.logSent = false;
		ml.log = "show machine " + ull(id);
		ml.run = [sm = ctx.showMachine, id] { if (sm) { sm(id); } return true; };
		h.actions.push_back(ml);
		const bool automatic = ctx.producerAutomatic && ctx.producerAutomatic(id);
		const std::string cursor = ctx.producerCursor ? ctx.producerCursor(id) : "mine";
		const bool machine = t && t->kind == simlink::Interactable::kMachine;
		if (machine && t->hasMachine && t->machineKind == 3)	// a turret: the original's RMB mans it (k16.6.19); the sim has no order for it (event 49 / the mounted tail are its only wire)
		{
			order_rows(h, "man_turret", "man_turret_queue", "Man " + name, cursor, "man turret " + ull(id), [](bool) { return false; }, false, "the sim has no order to man a placed turret (reported)");
		}
		else if (machine && automatic && !(t->hasMachine && t->machineKind == 4))	// an automatic converter / power building: the original's RMB is OPERATE_AUTOMATIC_MACHINERY (152), not in the sim
		{
			order_rows(h, "auto_machine", "auto_machine_queue", "Operate " + name, cursor, "automatic machine " + ull(id), [](bool) { return false; }, false, "an automatic machine takes no operate order (the original's 152 is not in the sim)");
		}
		else if (automatic)	// the original's furnace RMB is LOOT_TARGET: open it, never a use the sim refuses (no_slot)
		{
			order_rows(h, "open_automatic", "producer_queue", "Open " + name, cursor, "open automatic producer " + ull(id),
				[po = ctx.producerOpen, id](bool q) { return po && po(id, q); });
		}
		else
		{
			order_rows(h, "use_producer", "producer_queue", std::string(cursor == "mine" ? "Mine " : "Operate ") + name, cursor, "use producer " + ull(id),
				[pu = ctx.producerUse, id](bool q) { return pu && pu(id, q); });
		}
		h.rule = "producer -> " + cursor + (cursor == "mine" ? " (world resource mining != 0, or a wild deposit)" : " (a machine without mining: gears)");
		return;
	}
	if (p.kind == Pick::Container && t)
	{
		const bool locked = t->state == simlink::Interactable::kLocked;
		const bool foreign = ctx.sim.foreign(id);
		const auto open = [co = ctx.containerOpen, id](bool q) { return co && co(id, q); };
		if (ctx.labelId == id)	// the label's click wins over the pick under it
		{
			Action lc;
			lc.gesture = Gesture::kClick;
			lc.kind = locked ? "lockpick_label" : "open_label";
			lc.label = (locked ? "Pick the lock of " : "Open ") + name;
			lc.log = std::string("label: ") + (locked ? "lockpick " : "open container ") + ull(id);
			lc.run = [open, order3 = order(3), locked, q = ctx.shift] { return locked ? order3(q) : open(q); };
			h.actions.push_back(lc);
		}
		Action lm;	// a left click: the window alone from anywhere, the selection kept (user 2026-11-02); a locked one picks
		lm.gesture = Gesture::kClick;
		if (locked)
		{
			lm.kind = "lockpick";
			lm.label = "Pick the lock of " + name;
			lm.log = "lockpick " + ull(id);
			lm.run = [order3 = order(3), q = ctx.shift] { return order3(q); };
		}
		else
		{
			lm.kind = "show_container";
			lm.label = "Show the window of " + name;
			lm.logSent = false;
			lm.log = "show container " + ull(id);
			lm.run = [cs = ctx.containerShow, id] { if (cs) { cs(id); } return true; };
		}
		h.actions.push_back(lm);
		const std::string cursor = locked ? "lockpick" : foreign ? "scavenge" : "search";
		if (foreign && !ctx.alt)
		{
			// the original's hint and (orders.foreign_walk_to) the move at the same instant; no loot, no lockpick (ALT does those; user 2026-11-14, kenshi-k16.2.240.2)
			const std::string what = std::string("foreign ") + (locked ? "locked container " : "container ") + ull(id);
			const std::string tail = std::string(", no ALT: hint") + (ctx.foreignWalking ? " + walk, no steal/lockpick" : " only");
			order_rows(h, "foreign_walk", "foreign_walk_queue", ctx.foreignWalking ? "Walk to " + name + (locked ? " (hold ALT to pick the lock)" : " (hold ALT to steal)") : "Hint only: hold ALT on " + name + (locked ? " to pick the lock" : " to steal"),
				cursor, what + tail, [fw = ctx.foreignWalk, id](bool q) { return fw && fw(id, q); }, true, "", what + " (queued)" + tail);
		}
		else if (locked) { order_rows(h, "lockpick", "lockpick_queue", "Pick the lock of " + name, cursor, "lockpick " + ull(id), order(3)); }
		else { order_rows(h, "open_container", "open_container_queue", "Open " + name, cursor, "open container " + ull(id), open); }
		h.target += foreign ? (locked ? " (foreign, locked)" : " (foreign)") : locked ? " (locked)" : "";
		h.rule = std::string("container -> ") + (locked ? "locked: lockpick" : foreign ? "foreign (not ours): scavenge" : "ours: search");
		return;
	}
	if (p.kind == Pick::Door && t)
	{
		// locked: the click does nothing; the hold menu has Pick Lock (order 60 action 3), unlock and bash. Broken: nothing to do. Damaged: the spanner
		// cursor, the click still opens or closes
		const bool locked = t->state == simlink::Interactable::kLocked, broken = t->state == simlink::Interactable::kBroken;
		const bool damaged = t->health < t->maxHealth;
		const std::string cursor = locked ? "lock" : damaged ? "spanner" : "door";
		if (locked || broken)
		{
			Action no;
			no.gesture = Gesture::kRightClick;
			no.kind = locked ? "door_locked" : "door_broken";
			no.label = (locked ? "Locked: hold right click for Unlock, Pick Lock or Bash" : "Broken: nothing to open");
			no.enabled = false;
			no.why = locked ? "locked" : "broken";
			no.cursor = cursor;
			no.log = "door " + ull(id) + (locked ? " locked" : " broken");
			no.run = [] { return false; };
			h.actions.push_back(no);
		}
		else
		{
			const bool open = t->state == simlink::Interactable::kOpen;
			order_rows(h, open ? "door_close" : "door_open", "door_queue", std::string(open ? "Close " : "Open ") + name + (damaged ? " (damaged)" : ""), cursor,
				"door " + ull(id) + (open ? " close" : " open"), order(open ? 2 : 1));
		}
		h.target += locked ? " (locked)" : broken ? " (broken)" : damaged ? " (damaged)" : t->state == simlink::Interactable::kOpen ? " (open)" : " (closed)";
		h.rule = "door -> " + std::string(locked ? "locked: lock" : damaged ? "damaged: spanner (RMB still opens)" : "intact: door");
	}
}
}

HINT_KIND("move", "the ground is a move order");
HINT_KIND("move_queue", "queues the rest");
HINT_KIND("deselect", "A left click on the ground or on nothing clears the selection");

Action deselect_action(const Context& ctx)
{
	Action a;
	a.gesture = Gesture::kClick;
	a.kind = "deselect";
	a.label = "Clear the selection (Shift keeps it)";
	a.run = [cs = ctx.clearSelect, sh = ctx.shift] { if (cs) { cs(sh); } return true; };
	a.logSent = false;
	a.after = [&sim = ctx.sim](bool) { return "character 0, " + std::to_string(sim.selected().size()) + " selected"; };
	return a;
}

namespace
{
// The ground and a player's building (step 8): the right click moves the selection to the hit (Shift queues), the left click clears the selection. A player's
// building changes only the cursor (CURSORS.md "Building hover", 1407fc6e0): build while it is not built, spanner when damaged (UNSURE there), else the class's own
// cursor, which the wire does not carry: the fallback use (9). A foreign building's shape sets no cursor (the original's hover candidate); terrain with no navmesh
// under it shows invalidmove, and the right click still sends the order.
void ground_hints(Hover& h, const Pick& p, const Context& ctx)
{
	std::string cursor;
	if (const simlink::SimLinkDecoder::Building* pb = p.building && p.id ? ctx.sim.building(p.id) : nullptr)
	{
		cursor = !(pb->flags & simlink::SimLinkDecoder::Building::kBuilt) ? "build" : pb->hp < pb->maxHp ? "spanner" : "use";
		h.rule = std::string("player's building -> ") + (cursor == "build" ? "not built: build" : cursor == "spanner" ? "damaged: spanner" : "built, class cursor unknown: use");
	}
	else if (!p.building && ctx.walkable && ctx.walkable(p.hit) == 0) { cursor = "invalidmove"; h.rule = "terrain / foliage with no navmesh under it (walkable = no) -> invalidmove"; }
	else if (p.building) { h.rule = "building shape -> arrow (a foreign building sets no cursor: the original's hover candidate)"; }
	else { h.rule = std::string("ground, walkable ") + (ctx.walkable && ctx.walkable(p.hit) == 1 ? "yes" : "unknown") + " -> arrow (a right click moves there)"; }
	// the click's whole log line is the move's own (it carries the snapped point), so the rows share one text slot the run fills and `after` reads
	const auto text = std::make_shared<std::string>();
	order_rows(h, "move", "move_queue", "Move here", cursor, "", [gm = ctx.groundMove, text](bool queue) { return gm && gm(queue, *text); });
	for (Action& a : h.actions) { a.logSent = false; a.after = [text](bool) { return *text; }; }
	h.actions.push_back(deselect_action(ctx));
}
}

HINT_KIND("menu_follow", "a character (Follow, Bodyguard");
HINT_KIND("menu_bodyguard", "a character (Follow, Bodyguard");
HINT_KIND("menu_talk", "Talk / Attack");
HINT_KIND("menu_attack", "Talk / Attack");
HINT_KIND("menu_stealth_ko", "keeps Stealth KO");
HINT_KIND("menu_loot", "Loot, First aid");
HINT_KIND("menu_first_aid", "Loot, First aid");
HINT_KIND("menu_pick_up", "Pick up / Put down");
HINT_KIND("menu_put_down", "Pick up / Put down");
HINT_KIND("menu_open", "Open / Close, Unlock");
HINT_KIND("menu_close", "Open / Close, Unlock");
HINT_KIND("menu_unlock", "Unlock, Pick Lock, Bash");
HINT_KIND("menu_bash", "Unlock, Pick Lock, Bash");
HINT_KIND("menu_lockpick", "Unlock, Pick Lock, Bash");
HINT_KIND("menu_take_all", "Open, Take all, Pick");
HINT_KIND("menu_operate", "Mine or Operate");

namespace
{
// The hold-RMB menu rows (step 9, DESIGN 4: the deleted menu_items): every row is a HoldRight action with menu set, in the order the menu shows them; the title is
// Hover::menuTitle. A row whose order cannot be done is left out, never greyed. The row's log is the "sim menu:" line's text ("<what> <id>").
void menu_row(Hover& h, const char* kind, const std::string& text, const std::string& what, uint64_t id, std::function<bool()> run)
{
	Action m;
	m.gesture = Gesture::kHoldRight;
	m.kind = kind;
	m.label = text;
	m.menu = true;
	m.menuText = text;
	m.log = what + " " + std::to_string(id);
	m.run = std::move(run);
	h.actions.push_back(m);
}

void character_menu(Hover& h, uint64_t id, const Context& ctx)
{
	const SimView& sim = ctx.sim;
	const auto ch = sim.decoder().chars.find(id);
	const bool known = ch != sim.decoder().chars.end();
	const bool down = known && (ch->second.cur.flags & (simlink::kKO | simlink::kDead));
	h.menuTitle = char_name(id, ctx);
	const auto order = [co = ctx.charOrder, id](int action) { return [co, id, action] { return co && co(id, action, false); }; };
	const auto bodyguard = [&] { menu_row(h, "menu_bodyguard", "Bodyguard", "bodyguard", id, [cj = ctx.charJob, id] { return cj && cj(45, id); }); };
	if (sim.isPlayer(id))
	{
		menu_row(h, "menu_follow", "Follow", "follow", id, order(kFollow));
		if (!down) { bodyguard(); }
	}
	else
	{
		if (!down)
		{
			// Talk To only when the server's gate passes (MENU_ENTRIES bit 0, the answer to order 99; SIMLINK MENU_ENTRIES)
			if (ctx.entries < 0 || (ctx.entries & 1)) { menu_row(h, "menu_talk", "Talk", "talk", id, order(6)); }
			bodyguard();
		}
		menu_row(h, "menu_attack", "Attack", "attack", id, order(kAttack));
		// Stealth KO stays offered for every living, standing target while a selected character sneaks (user 2026-11-11)
		if (!down && sneak_actor(sim)) { menu_row(h, "menu_stealth_ko", "Stealth KO", "stealth ko", id, order(14)); }
	}
	if (down) { menu_row(h, "menu_loot", "Loot", "loot", id, order(5)); }
	// first aid (needs a kit in the medic's inventory; the server answers otherwise): anyone down, and our own when hurt
	if (down || (sim.isPlayer(id) && sim.health(id, ctx.chars) < 1.0f)) { menu_row(h, "menu_first_aid", "First aid", "first aid", id, order(10)); }
	// carrying (order 60 action 8 lifts a KO'd / dead character, 13 puts the carried one down; the server picks the carrier for 13)
	const bool carried = known && ch->second.cur.carriedBy != 0;
	if (carried) { menu_row(h, "menu_put_down", "Put down", "put down", id, order(13)); }
	else if (down) { menu_row(h, "menu_pick_up", "Pick up", "pick up", id, order(8)); }
	// the carrier's menu puts its load down too (the carried body rides on the carrier, so a click on it picks the carrier)
	for (const auto& [cid, cc] : sim.decoder().chars)
	{
		if (cc.cur.carriedBy != id || !sim.isPlayer(id)) { continue; }
		menu_row(h, "menu_put_down", "Put down", "put down", cid, [co = ctx.charOrder, cid] { return co && co(cid, 13, false); });
	}
}

void thing_menu(Hover& h, uint64_t id, const Context& ctx)
{
	const simlink::Interactable* t = ctx.sim.thing(id);
	if (!t) { return; }
	std::string name = ctx.sim.nameOf(t->record, ctx.chars);
	if (t->kind == simlink::Interactable::kItem && t->count > 1) { name += " (" + std::to_string(t->count) + ")"; }
	if (t->kind == simlink::Interactable::kDoor && t->state == simlink::Interactable::kBroken) { name += " (broken)"; }
	h.menuTitle = name;
	const auto order = [to = ctx.thingOrder, id](int action) { return [to, id, action] { return to && to(id, action, false); }; };
	const auto open = [co = ctx.containerOpen, id] { return co && co(id, false); };
	if (t->kind == simlink::Interactable::kItem) { menu_row(h, "menu_pick_up", "Pick up", "pick up", id, order(7)); }
	else if (t->machineLike())
	{
		// an automatic machine (max operators 0: the sim answers no_slot to a use) offers no operate row; its right click is the loot (Open).
		// A kind 12 machine opens only when it holds contents (the furnace: machineKind 4); a turret's man row waits on a sim order.
		const bool kind12 = t->kind == simlink::Interactable::kMachine;
		const bool automatic = ctx.producerAutomatic && ctx.producerAutomatic(id);
		const bool turret = kind12 && t->hasMachine && t->machineKind == 3, furnace = kind12 && t->hasMachine && t->machineKind == 4;
		if (!automatic && !turret)
		{
			const bool mine = ctx.producerCursor && ctx.producerCursor(id) == "mine";
			menu_row(h, "menu_operate", mine ? "Mine" : "Operate", "use", id, [pu = ctx.producerUse, id] { return pu && pu(id, false); });
		}
		if (!kind12 || furnace) { menu_row(h, "menu_open", "Open", "open", id, open); }
	}
	else if (t->kind == simlink::Interactable::kContainer)
	{
		if (t->state == simlink::Interactable::kLocked)
		{
			// a locked box: the pick (order 60 action 3, SIMLINK's kind 3 lock); "Take all" stays so the sim answers the loot itself
			menu_row(h, "menu_lockpick", "Pick Lock", "lockpick", id, order(3));
			menu_row(h, "menu_take_all", "Take all", "take all", id, order(5));
		}
		else
		{
			menu_row(h, "menu_open", "Open", "open", id, open);
			menu_row(h, "menu_take_all", "Take all", "take all", id, order(5));
		}
	}
	else if (t->kind == simlink::Interactable::kDoor)
	{
		const bool shut = t->state == simlink::Interactable::kLocked || t->state == simlink::Interactable::kClosed;
		if (t->state == simlink::Interactable::kClosed) { menu_row(h, "menu_open", "Open", "open", id, order(1)); }
		if (t->state == simlink::Interactable::kOpen) { menu_row(h, "menu_close", "Close", "close", id, order(2)); }
		// unlock: the server allows the door's owner faction only; bash: any shut door, until it breaks (state 3)
		if (t->state == simlink::Interactable::kLocked)
		{
			menu_row(h, "menu_unlock", "Unlock", "unlock", id, order(11));
			menu_row(h, "menu_lockpick", "Pick Lock", "lockpick", id, order(3));
		}
		if (shut) { menu_row(h, "menu_bash", "Bash", "bash", id, order(12)); }
	}
}
}

Hover hints_for(const Pick& p, const Context& ctx)
{
	Hover h;
	h.target = target_name(p, ctx);
	// Each step adds its kind's rows here and deletes the branch in hover_cursor / sim_click that decided them; the two must never
	// disagree. Done: portraits (hints_for_portrait), characters in the world (steps 2 and 3), items, beds, producers and doors (steps 4 and 5).
	if (p.kind == Pick::Character) { character_hints(h, p, ctx); }
	else if (p.kind == Pick::Item || p.kind == Pick::Bed || p.kind == Pick::Producer || p.kind == Pick::Door || p.kind == Pick::Container) { thing_hints(h, p, ctx); }
	else if (p.kind == Pick::Ground) { ground_hints(h, p, ctx); }
	return h;
}

const Action* match(const Hover& h, Gesture g, const Context& ctx)
{
	(void)ctx;	// the double-click pair lives in the dispatcher (it picks kDoubleRightClick when the pair matches and the target offers it)
	for (const Action& a : h.actions) { if (a.gesture == g) { return &a; } }
	const Gesture plain = g == Gesture::kShiftClick || g == Gesture::kCtrlClick || g == Gesture::kAltClick ? Gesture::kClick
		: g == Gesture::kShiftRightClick || g == Gesture::kCtrlRightClick || g == Gesture::kAltRightClick ? Gesture::kRightClick : g;
	if (plain != g) { for (const Action& a : h.actions) { if (a.gesture == plain) { return &a; } } }
	return nullptr;
}

std::string format_hover(const Hover& h)
{
	std::string out = "hints: " + (h.target.empty() ? "(no target)" : h.target) + ": " + std::to_string(h.actions.size()) + (h.actions.size() == 1 ? " action" : " actions") + "\n";
	for (const Action& a : h.actions)
	{
		out += std::string("  ") + gesture_name(a.gesture) + "  " + a.kind + "  \"" + a.label + "\"";
		if (!a.enabled) { out += "  disabled: " + (a.why.empty() ? "no reason given" : a.why); }
		if (a.menu) { out += "  (menu row)"; }
		if (a.key) { out += std::string("  key ") + a.key; }
		out += "\n";
	}
	return out;
}

const std::vector<HintKind>& hint_kinds() { return hint_registry(); }

void register_hint_kind(const char* id, const char* phrase)
{
	std::vector<HintKind>& kinds = hint_registry();
	for (const HintKind& k : kinds)
	{
		if (std::string(k.id) == id) { fprintf(stderr, "kclient: HINT_KIND: \"%s\" registered twice (\"%s\", then \"%s\"): a kind has one home, keep the first\n", id, k.phrase, phrase); return; }
	}
	kinds.push_back({ id, phrase });
}
}
