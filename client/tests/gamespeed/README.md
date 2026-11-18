# gamespeed (kenshi-k16.2.181.1)

The 5x stutter check (the user: "At 5x speed, the sim is extremely choppy. Your character stutters and jumps around. Outside squads moving
around are horribly choppy and jumpy"). The same bar_lure run at 1x, 2x and 5x, the Wanderer followed and walked south of the Hub while the
bar's residents and the Dust bandit patrol stream around it.

    client/tests/gamespeed/run.sh OUT [SPEEDS]

`FRAMES` (default 1200) is the script's frame count; `PORT` (default 47836) the first of one port per speed. Each run is
`OUT/speed<N>/` with its server.log, client.log, script.txt and summary.txt, and the three RUN lines collect in `OUT/table.txt`.

`script.txt` sets `set framedt real` (a scripted run steps the world by each frame's real duration, not the fixed 1/60 s of a shot, so the
render clock keeps real time at any frame rate — the sim runs in real time), the speed with `set simspeed` at frame 170, and keeps a move
order live along the Hub's south line (two points ~620 u apart, a new `move` every 60 frames), so the player keeps running at every speed.
`log pos all` (`client/src/dev/poslog.cpp`) writes one line per drawn character per frame:

    pos T DT ID TICK NEWEST PAUSED SIMx SIMy SIMz DRAWNx DRAWNy DRAWNz STEP WALL SNAPS

`SIM` is the render clock's interpolated position, `DRAWN` the position the body is drawn at (the display slop included), `TICK` the render
clock, `NEWEST` the newest complete snapshot's tick, `STEP` how far the drawn moved since that character's previous logged frame, `WALL`
the steady clock since the op started, `SNAPS` the decoder's SNAPSHOT_END count (its rise between two frames is how many snapshots that
frame took in).

`summary.awk` takes the statistics over moving frames (the sim moved the character; > 0.05 u) so standing between orders does not dilute
them: per character the median and the maximum `STEP` and their ratio, the jump count (`STEP` > 2.5 x the median) and the stall count (the
sim moved, `STEP` < 0.25 x the median), the lag `NEWEST - TICK` in ticks and wall ms, the render clock's advance per frame, and the harness
share: the part of the drawn travel where the clock advanced faster than real time (`wall dt x 20 x speed` ticks a frame), i.e. the hold's
catch-up, which a live client does not have. `run.sh` also prints each run's mean frame wall time and its received snapshot rate against
the sim's own 20 x speed ticks a wall second.

With `set framedt real` the harness share is near 0; a scripted *shot* keeps the fixed 1/60 s clock (`kScriptDt`, `main.cpp`), which at a
frame rate below 60 fps leaves the render clock behind the real-time sim and turns the hold's catch-up into drawn jumps — always compare
runs made the same way.

The smooth drawn step a frame is the character's speed x the frame's wall time (70 u/s run: 1.17 u a 60 fps frame at 1x, 5.83 at 5x): a
jump reads well over it.

Ports: `PORT` (default 47836), one per speed in the list. `SIM` runs the measurement against another server binary — the
`KSIM_PROFILE` build with `KSIM_PROFILE_JSON=1` in the environment prints the sim's own `tick_us` and section means on its stderr
(`server.log`), which is how the 5x run attributed the uneven sends to the snapshot encode (kenshi-k16.2.181.1).
