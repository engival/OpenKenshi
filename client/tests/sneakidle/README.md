# sneakidle (kenshi-k16.2.160.1)

`run.sh OUT`: starts OpenKenshi-server on scenarios/bar_lure_stealth_arrows (the Wanderer sneaks, sword in hand) and a headless kclient; `dump anim` must
show `crouch idle` (the `stealth idle` record) loaded sneaking, `stand 1 sword` after X (sneak off), `crouch idle` after X again. Exit 1 and a FAIL line
otherwise. Shots a_loaded / b_off / c_on in OUT.

`combat.sh OUT` (kenshi-k16.2.162.1): scene characters with a Desert Sabre / Iron Club in combat; `dump idle` must show `guard5` and `guard 1h` (the combat picker is the first maximum
in the original's sid hash order, sidorder.hpp; the logged picks of rwh.101). The stock idle list positions are logged at start (`anim: idle list order`).
`anim.idle_draw` (default 0): 1 makes the standing idle the original's weighted random draw held for `idle time` (every record has 10..40 here, so a sneaking armed character
then flips between crouch and stand: why 0 is the default).
