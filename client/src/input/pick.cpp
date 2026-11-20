// pick.hpp's predicates, moved out of main.cpp (kenshi-k16.2.294.2, control-hints step 0). The code is main.cpp's, line for line: only `app.sim`
// became `sim`, `app.chars` `chars` and the Ctrl test a `ctrl` argument.
#include "input/pick.hpp"

#include <algorithm>

#include "characters/characters.hpp"
#include "bridge/simview.hpp"

uint64_t sneak_actor(const SimView& sim)
{
	const auto& chars = sim.decoder().chars;
	for (uint64_t s : sim.selected())
	{
		const auto c = chars.find(s);
		if (c != chars.end() && (c->second.cur.flags & simlink::kStealth) && !(c->second.cur.flags & (simlink::kCarryL | simlink::kCarryR))) { return s; }
	}
	return 0;
}

bool seen_by(const SimView& sim, uint64_t id, uint64_t observer)
{
	const auto& sb = sim.decoder().seenBy;
	const auto it = sb.find(id);
	return it != sb.end() && std::find(it->second.by.begin(), it->second.by.end(), observer) != it->second.by.end();
}

const simlink::SimLinkDecoder::Cage* locked_cage(const SimView& sim, uint64_t id)
{
	const auto& cages = sim.decoder().cages;
	const auto it = cages.find(id);
	return it != cages.end() && it->second.locked ? &it->second : nullptr;
}

bool container_locked(const SimView& sim, uint64_t id)
{
	const simlink::Interactable* t = sim.thing(id);
	return t && t->kind == simlink::Interactable::kContainer && t->state == simlink::Interactable::kLocked;
}

CharAct char_act(const SimView& sim, const Characters& chars, uint64_t id, bool ctrl)
{
	const auto ch = sim.decoder().chars.find(id);
	if (ch == sim.decoder().chars.end()) { return {}; }
	const uint32_t flags = ch->second.cur.flags;
	const auto& sel = sim.selected();
	const bool other = std::any_of(sel.begin(), sel.end(), [&](uint64_t s) { return s != id; });
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
