-- The economy's engine rules, as the shipped default script (TOWN_LIFE_SLICE 7.8 / 7.13; LUA_API.md "The economy hooks").
-- The engine runs every rule itself when no script is loaded; this file is the starting point a mod copies and edits. Its price rule
-- is the engine's stockFactor, written out: clamp((target / max(stock, 1))^k, lo, hi), k = 1, lo = 0.5, hi = 2.
local K = 1.0
local LO = 0.5
local HI = 2.0
local FLOOR = 1.0

local M = { api = 0 }

-- economy.price(item, town, stock, target): the factor both sides of a quote take. > 1 while the item is scarce, < 1 with a glut.
function M.price(q)
	local amount = q.stock
	if amount < FLOOR then amount = FLOOR end
	local factor = (q.target / amount) ^ K
	if factor < LO then return LO end
	if factor > HI then return HI end
	return factor
end

-- economy.demand(town): the town's target levels after the script; an empty table keeps the stock's own targets.
function M.demand(q)
	return {}
end

return M
