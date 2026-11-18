// controlhints.hpp: the gesture names, the kind registry and the (empty) resolver (kenshi-k16.2.294.2, control-hints step 0). Read the header first.
#include "input/controlhints.hpp"

#include <algorithm>
#include <cstdio>

#include "characters/characters.hpp"
#include "net/simview.hpp"

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

Hover hints_for(const Pick& p, const Context& ctx)
{
	Hover h;
	h.target = target_name(p, ctx);
	// Step 0: no kind has moved into the resolver yet (DESIGN 4 lists the order: portraits first). Each step adds its rows here and deletes the
	// branch in hover_cursor / sim_click / menu_items that decided them; the two must never disagree.
	return h;
}

const Action* match(const Hover& h, Gesture g, const Context& ctx)
{
	(void)ctx;	// the double-click pair and the modifier rules arrive with the first resolver
	for (const Action& a : h.actions) { if (a.gesture == g) { return &a; } }
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
