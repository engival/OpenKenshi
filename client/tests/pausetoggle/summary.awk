# pausetoggle summary (kenshi-k16.2.180.1; the player verdict kenshi-k16.2.192): the tails of a `log pos all` client.log, per character
# (client/src/dev/poslog.cpp).
# Line: pos T DT ID TICK NEWEST PAUSED SIMx SIMy SIMz DRAWNx DRAWNy DRAWNz STEP
# fields: 1 pos, 2 t, 3 dt, 4 dtValue, 5 id, 6 idValue, 7 tick, 8 tickValue, 9 newest, 10 newestValue, 11 paused, 12 pausedValue,
# 13 sim, 14..16 sim x/y/z, 17 drawn, 18..20 drawn x/y/z, 21 step, 22 stepValue (pausedValue 1 = paused; 0 = running)
# `step` is a character's drawn move since its own previous logged frame. While paused the clock and the sim freeze, so every paused
# frame after the first must be 0.000; the first paused frame still carries the last running frame's move (the pause lands after that
# interval), so it is reported apart. A rock is a frame where the drawn stepped against the sim's own move that frame.
# -v player=ID: the followed player character (run.sh takes it from `dump chars`). Its SIM path length is the walk the two `move`
# orders ask for; with no walk the paused verdict only covers standing characters, so the walk is part of the pass/fail and the exit code.
BEGIN { pnum = player + 0 }
$1 != "pos" { next }
{
	id = $6; paused = ($12 == 1); step = $22 + 0; t = $2 + 0
	sx = $14 + 0; sz = $16 + 0; dx = $18 + 0; dz = $20 + 0
	n[id]++
	if (paused) {
		if (pst[id]) { if (step > mp[id]) { mp[id] = step; mpt[id] = t } }
		else { if (step > me[id]) { me[id] = step; met[id] = t } }
		pst[id] = 1
	}
	else { if (step > mr[id]) { mr[id] = step; tr[id] = t } pst[id] = 0 }
	if (step > all) { all = step; allId = id; allT = t; allP = paused }
	if (id in psx) {
		msx = sx - psx[id]; msz = sz - psz[id]; mdx = dx - pdx[id]; mdz = dz - pdz[id]
		sm = sqrt(msx * msx + msz * msz); dm = sqrt(mdx * mdx + mdz * mdz)
		# the sim moved this frame and the drawn moved against it: a back-and-forth (a slop ease along the sim s own line is not)
		if (sm > 0.05 && dm > 0.2 && (msx * mdx + msz * mdz) < -0.2 * sm * dm) { rock[id]++; if (dm > rockMax[id]) { rockMax[id] = dm; rockT[id] = t } }
		# the player's own walked path (SIM), the test's walk window: every frame's sim move added up
		if (id == pnum) { walked += sm; if (!pstart) { px0 = psx[id]; pz0 = psz[id]; pstart = 1 } px1 = sx; pz1 = sz; pframes++ }
	}
	psx[id] = sx; psz[id] = sz; pdx[id] = dx; pdz[id] = dz
}
END {
	pnum = player + 0
	printf "id        frames  paused after entry (max)  pause entry (max, t)  running max (t)  rock frames (max, t)\n"
	for (id in n) { printf "%-8s  %6d  %15.3f  %13.3f (%s)  %13.3f (%s)  %6d (%.3f, %s)\n", id, n[id], mp[id], me[id], met[id], mr[id], tr[id], rock[id], rockMax[id], rockT[id] }
	printf "largest drawn step overall: %.3f u (id %s, t %.3f, %s)\n", all, allId, allT, allP ? "paused" : "running"
	# The walk is what the mid-walk toggles are for: with no walk the paused counts only cover standing characters (kenshi-k16.2.192).
	if (!pnum || !pframes) {
		printf "player %s: no pos lines (run.sh takes the id from `dump chars`)\n", player
		printf "pausetoggle: FAIL (the followed player character was not logged)\n"
		exit 1
	}
	net = sqrt((px1 - px0) * (px1 - px0) + (pz1 - pz0) * (pz1 - pz0))
	printf "player %s: walked %.1f u along the route (net %.1f u) over %d logged frames; running step max %.3f (t %s), pause-entry max %.3f (t %s), paused-after-entry max %.3f, rocks %d\n",
		player, walked, net, pframes, mr[pnum], tr[pnum], me[pnum], met[pnum], mp[pnum], rock[pnum]
	why = ""
	if (walked < 20) { why = why " the move orders did not land (walked under 20 u);" }
	if (mp[pnum] > 0.01) { why = why sprintf(" a paused frame moved %.3f u;", mp[pnum]) }
	if (rock[pnum] > 0) { why = why sprintf(" %d rock frame(s);", rock[pnum]) }
	if (why == "") {
		printf "pausetoggle: PASS (the player walked the walk window; no paused move, no rock)\n"
		exit 0
	}
	printf "pausetoggle: FAIL (%s)\n", why
	exit 1
}
