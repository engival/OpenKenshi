# gamespeed summary (kenshi-k16.2.181.1): one run's `log pos all` tail, per drawn character and a RUN line, from client/src/dev/poslog.cpp.
# Line: pos T DT ID TICK NEWEST PAUSED SIMx SIMy SIMz DRAWNx DRAWNy DRAWNz STEP WALL SNAPS
# fields: 1 pos, 2 t (sim-clock seconds), 3 dt, 4 dtValue, 5 id, 6 idValue, 7 tick, 8 tickValue (the render clock),
#         9 newest, 10 newestValue (the newest complete snapshot's tick), 11 paused, 12 pausedValue,
#         13 sim, 14..16 sim x/y/z, 17 drawn, 18..20 drawn x/y/z, 21 step, 22 stepValue, 23 wall, 24 wallValue (steady seconds),
#         25 snaps, 26 snapsValue (the SNAPSHOT_END count; run.sh reads its deltas for the arrival histogram).
# `step` is a character's drawn move since its own previous logged frame; the sim's own move over that frame is SIM's. The statistics are
# over moving frames only (the sim moved the character), so standing between orders does not dilute them. `clock` is the render clock's
# advance that frame, which the drawn follows; a scripted run advances it a fixed `base = speed/3` ticks a frame (1/60 s x 20 ticks/s x
# speed), and the excess over base is the clock hold's catch-up or a snap. A step's harness share is (clock - base) / clock: the part of
# the jump that a real 60 fps run (dt = the frame's true duration) would not have. `lag` = NEWEST - TICK: how far the render clock trails
# the newest snapshot, in ticks, and in wall ms (ticks / 20 / speed x 1000). The smooth drawn step a frame is 70 x speed / 60 u for the
# run gait (1.17 u at 1x, 5.83 at 5x); a 0.05 u frame gate separates moving from standing.
# Input: -v speed=N the run's game speed; -v player=ID the followed player (a `dump players` line names it if this is left empty).
$1 == "player" && player == "" { if (!($2 in seen)) { player = $2 } ; seen[$2] = 1 }
$1 != "pos" { next }
{
	id = $6
	n = ++frames[id]
	idx = id SUBSEP n
	tick = $8 + 0; newest = $10 + 0; step = $22 + 0
	sx = $14 + 0; sz = $16 + 0
	# the previous frame's sim position and render tick, per character (the frame's lines carry every character in turn)
	if (n == 1) { psx[id] = sx; psz[id] = sz; ptick[id] = tick; pwall[id] = $24 + 0 }
	simStep = sqrt((sx - psx[id]) * (sx - psx[id]) + (sz - psz[id]) * (sz - psz[id]))
	steps[idx] = step; simSteps[idx] = simStep; clocks[idx] = tick - ptick[id]; lags[idx] = newest - tick
	wallDts[idx] = ($24 + 0) - pwall[id]
	psx[id] = sx; psz[id] = sz; ptick[id] = tick; pwall[id] = $24 + 0
}
# histogram median over frames lo..n of an array (bucket 0.01, capped at 200)
function qmedian(id, name, lo, hi,   i, v, b, cum, total)
{
	total = 0
	for (i = lo; i <= hi; i++)
	{
		v = name == "step" ? steps[id SUBSEP i] : name == "lag" ? lags[id SUBSEP i] : clocks[id SUBSEP i]
		if (v < 0) { v = 0 }
		b = int(v * 100 + 0.5); if (b > 20000) { b = 20000 }
		h[id SUBSEP name SUBSEP b]++
		total++
	}
	cum = 0
	for (b = 0; b <= 20000; b++)
	{
		cum += h[id SUBSEP name SUBSEP b]
		if (cum * 2 >= total) { return b / 100.0 }
	}
	return 0
}
END {
	printf "id        who     frames  moving  step med   step max  ratio  jumps  stalls  lag med  lag max  lag max ms  clock med  clock max  worst frame clock  harness share\n"
	for (id in frames)
	{
		if (frames[id] < 30) { continue }
		mvN = 0
		for (i = 2; i <= frames[id]; i++)
		{
			if (simSteps[id SUBSEP i] > 0.05) { mv[++mvN] = i }
		}
		if (mvN < 30) { continue }
		# the moving frames' steps, re-indexed 1..mvN in the same histogram arrays under the name "mstep"
		for (j = 1; j <= mvN; j++)
		{
			v = steps[id SUBSEP mv[j]]
			b = int(v * 100 + 0.5); if (b > 20000) { b = 20000 }
			hm[id SUBSEP b]++
			if (v > mx[id]) { mx[id] = v; mxAt[id] = mv[j] }
		}
		cum = 0
		for (b = 0; b <= 20000; b++) { cum += hm[id SUBSEP b]; if (cum * 2 >= mvN) { med = b / 100.0; break } }
		jumps = 0; stalls = 0
		for (j = 1; j <= mvN; j++)
		{
			v = steps[id SUBSEP mv[j]]
			if (v > 2.5 * med) { jumps++ }
			if (simSteps[id SUBSEP mv[j]] > 1.5 * med && v < 0.25 * med) { stalls++ }
		}
		lagMed = qmedian(id, "lag", 2, frames[id]); lagMax = 0; clockMax = 0
		for (i = 2; i <= frames[id]; i++)
		{
			if (lags[id SUBSEP i] > lagMax) { lagMax = lags[id SUBSEP i] }
			if (clocks[id SUBSEP i] > clockMax) { clockMax = clocks[id SUBSEP i] }
		}
		clockMed = qmedian(id, "clock", 2, frames[id])
		# the harness: the render clock should advance wall dt x 20 x speed ticks a frame (`set framedt real`); the part it advanced
		# faster is the clock hold's catch-up, drawn as motion the sim did not make. The share is over the moving frames' clock total.
		cs = clocks[id SUBSEP mxAt[id]]
		expect = wallDts[id SUBSEP mxAt[id]] * 20 * speed
		worstShare = (cs > expect && mx[id] > 1e-9) ? (cs - expect) / cs : 0
		clockSum = 0; excessSum = 0
		for (j = 1; j <= mvN; j++) { c = clocks[id SUBSEP mv[j]]; e = wallDts[id SUBSEP mv[j]] * 20 * speed; clockSum += c; if (c > e) { excessSum += c - e } }
		share = clockSum > 0 ? excessSum / clockSum : 0
		who = id == player ? "player" : "other"
		ratio = mx[id] / (med > 0 ? med : 1)
		printf "%-9s %-7s %6d  %6d  %9.3f  %9.3f  %5.2f  %5d  %6d  %7.2f  %7.2f  %10.1f  %9.2f  %9.2f  %16.2f  %13.2f\n", \
			id, who, frames[id], mvN, med, mx[id], ratio, jumps, stalls, lagMed, lagMax, lagMax / 20 / speed * 1000, \
			clockMed, clockMax, cs, share
		if (ratio > worstRatio) { worstRatio = ratio; worstId = id; worstMed = med; worstMax = mx[id] }
		if (id == player) { pMed = med; pMax = mx[id]; pRatio = ratio; pJumps = jumps; pStalls = stalls; pLagMed = lagMed; pLagMax = lagMax; pClockMax = clockMax; pShare = share }
		if (lagMax > worstLag) { worstLag = lagMax }
		ids++
	}
	printf "RUN speed=%s ids=%d player=%s | player: stepmed=%.3f stepmax=%.3f ratio=%.2f jumps=%d stalls=%d lagmed=%.2f lagmaxms=%.1f clockmax=%.2f harness=%.2f | worst ratio=%.2f (id %s, med %.3f max %.3f)\n", \
		speed, ids, (player == "" ? "?" : player), pMed, pMax, pRatio, pJumps, pStalls, pLagMed, pLagMax / 20 / speed * 1000, pClockMax, pShare, \
		worstRatio, worstId, worstMed, worstMax
}
