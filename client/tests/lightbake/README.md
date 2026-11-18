# lightbake (kenshi-jj3.1.1)

`run.sh OUT` runs `storm.txt` twice with `debug.light_probes` on, the camera at the Hub bar (the Storm House building), no sim.
run1 starts without a lightbake cache and bakes (`light_bake: ... (baked)`); run2 loads it (`(from the cache)`). Each run prints the
`dump light_bake` lines (grid, probes, usable, triangles, seconds, the mean sky level in the open and indoors) and writes `slices.txt`
(`dump light_slice N`: one horizontal layer as text, `#` unusable, `0`-`9` the sky level), and `probes_inside.png` / `probes_above.png`.
What to look for: the open probes read about 1 (the ground bounce adds a little), the room reads 0-1, brighter along the door and under the
hatch / windows, `#` inside the walls and slabs. The bake runs on its own thread (at most 4): a frame stall of more than a few hundred ms
only appears in the first world frame of both runs.
