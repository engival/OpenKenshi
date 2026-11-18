# Locked shackles menu (kenshi-h2v.15.2.1)

Scripts against `scenarios/shackled_escape.kscen` (a fresh OpenKenshi-server each run): `PORT=47837 client/tests/shackles/run.sh SCRIPT OUT`.
Ids move with the data, so the scripts name characters as `@Picker@`, `@Spare@`, `@Strong@` and the runner fills their ids in from a probe run's
`dump chars` (`client/tests/charids.sh`; `OUT/ids.txt`, `OUT/idprobe.log`); a name the scenario lacks stops the run with exit 4.
`select id=N` makes that player character the selection, `key I` opens its inventory, the shackles sit in the `boots` grid.

- `pick.txt`: Picker (lockpicking 40): the three rows (`menu.png`), Pick Lock, "Shackles unlocked" in the log.
- `cut.txt`: Spare (a hacksaw): Use Tools; the "Cutting: N" bar (`cut_mid.png`).
- `strength_weak.txt` / `strength_strong.txt`: Use Strength refused for Spare, "Shackles broken!" for Strong.
