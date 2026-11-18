# pausetoggle (kenshi-k16.2.180.1)

Pause toggled on and off every 0.5 s, standing, mid-walk and standing again, to check that no character moves or snaps when the sim
pauses and resumes (the user: "When you pause and restart (toggle back and forth), your character teleports back and forth in place,
rocking").

    client/tests/pausetoggle/run.sh OUT

`toggle.txt` drives it: the bar_lure scenario, the Wanderer followed (the follow camera), 10 toggles standing, two `move X Z`
world-target orders south and west of the Hub (the same order a click sends, with no pointer pixel to aim) and 20 toggles while it
walks, then 10 standing toggles. `log pos all` (dev op, `client/src/dev/poslog.cpp`) writes one line per drawn character per frame to
`OUT/client.log`:

    pos T DT ID TICK NEWEST PAUSED SIMx SIMy SIMz DRAWNx DRAWNy DRAWNz STEP

`SIM` is the render clock's interpolated position (what gameplay reads), `DRAWN` the position the body is drawn at (the display
slop included, `simview.cpp` Slot::pos), `TICK` the render clock and `NEWEST` the newest complete snapshot's tick, `STEP` how far
the drawn moved since that character's previous logged frame. The pause lands between rendered frames, so the first `PAUSED 1`
line still carries that frame's running move; every `STEP` after it must be 0.000 until the resume, and across a toggle no step
may exceed the gait's own.

`run.sh` pipes the log through `summary.awk`: per character the largest step on any frame after the pause landed (`PAUSED 1`,
`DT 0`; must be 0.000), on the pause-entry frame (`PAUSED 1`, `DT > 0`, the last running move), and while running; then the frames
where the drawn stepped against the sim's own move (a back-and-forth), and the largest step overall. The headless run's script
clock advances 1/60 s a frame while the sim runs in real time, so the render clock catches up in jumps: a running step of
0.2-0.7 u is that catch-up, forward along the gait, not a rock. `OUT/stand_start.png` and `OUT/walk_end.png` are shots of the
start and the end.

The ids are the sim's, so the player's is not the scenario file's `char id=1`: `run.sh` takes it from the `dump chars` line of the
followed Wanderer (the only player-controlled character, the `0x40` flag; its wire id moves with the scenario (the run prints it), while 6644 is the Bar's Barman) and ends with
that character's walked distance and a verdict. Without a walk the paused counts only cover standing characters, so the verdict
fails (`pausetoggle: FAIL`) and the script's exit code is 1; `pausetoggle: PASS` means the two move orders landed and every mid-walk
toggle held the drawn position. The server runs with `--order-log OUT/orders.log`, so `run.sh` also prints the two `move_to` orders
the server accepted for the player's id (none means the client never sent them, or the sim refused them as `no_route`).

Port: `PORT` (default 47835).
