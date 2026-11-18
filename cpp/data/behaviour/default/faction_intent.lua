-- Default faction temperament and intent (docs/reference/LUA_API.md).
--
-- PROVISIONAL, ours: the utility layer over the exe-grounded campaign roll. Every
-- number below is a guess (docs/reference/FACTIONS.md). The roll, the campaign
-- lifecycle and the relation reasons stay in C++: they are the original's formulas.
--
-- Each game-hour the temperament decays, this hour's losses and gains are added, and a
-- small goal set is scored by utility; the intent in hand keeps a fading bonus. The
-- order of the arithmetic is the order of the engine's native fallback
-- (UtilityFactionLayer::nativeDecide) on purpose: the two must agree to the bit.

local ALARM_DECAY = 0.97 -- per game-hour (a half-life of about a day)
local GRIEF_DECAY = 0.98
local CONFIDENCE_DECAY = 0.97

local SQUAD_LOST_GRIEF = 0.12 -- what an event adds
local TOWN_LOST_ALARM = 0.6
local TOWN_LOST_GRIEF = 0.25
local TOWN_GAINED_CONFIDENCE = 0.4

local CONSOLIDATE_BASE = 0.35
local CONSOLIDATE_CONFIDENCE = 0.5
local DEFEND_BASE = 0.2
local DEFEND_ALARM = 1.3
local MOURN = 1.2
local RAID_SCALE = 0.9
local INTENT_BONUS = 0.1 -- the intent in hand keeps this, fading over...
local INTENT_BONUS_HOURS = 6.0

-- In tie-break order: the first of equal scores wins, and Consolidate is always possible.
local INTENTS = { "consolidate", "defend", "raid", "mourn" }

return {
	api = 0,

	subscriptions = {
		{ on = "timer", every_hours = 1 },
		{ on = "squad_lost" },
		{ on = "town_lost" },
		{ on = "town_gained" },
	},

	decide = function(view)
		local alarm = view.alarm * ALARM_DECAY
		local grief = view.grief * GRIEF_DECAY
		local confidence = view.confidence * CONFIDENCE_DECAY
		for i = 1, #view.events do
			local kind = view.events[i].on
			if kind == "squad_lost" then
				grief = grief + SQUAD_LOST_GRIEF
			elseif kind == "town_lost" then
				alarm = alarm + TOWN_LOST_ALARM
				grief = grief + TOWN_LOST_GRIEF
			elseif kind == "town_gained" then
				confidence = confidence + TOWN_GAINED_CONFIDENCE
			end
		end
		alarm = math.min(1.0, alarm)
		grief = math.min(1.0, grief)
		confidence = math.min(1.0, confidence)

		local score = {
			consolidate = CONSOLIDATE_BASE + CONSOLIDATE_CONFIDENCE * confidence,
			defend = DEFEND_BASE + DEFEND_ALARM * alarm,
			raid = 0.0,
			mourn = MOURN * grief,
		}
		if view.can_raid then
			score.raid = view.aggression * (1.0 - grief) * RAID_SCALE
		end
		score[view.intent] = score[view.intent]
			+ INTENT_BONUS * math.max(0.0, 1.0 - view.intent_hours / INTENT_BONUS_HOURS)

		local best = INTENTS[1]
		for i = 2, #INTENTS do
			if score[INTENTS[i]] > score[best] then
				best = INTENTS[i]
			end
		end
		return {
			set_temperament = { alarm = alarm, grief = grief, confidence = confidence },
			set_intent = best,
		}
	end,
}
