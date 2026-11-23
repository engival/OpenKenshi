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
