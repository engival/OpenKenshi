# boxselect check (kenshi-k16.2.230.3): reads client.log and says whether the selection box follows the original's rules (kenshi-rwh.134).
# The client lines read here:
#   box select frame=F rect=X0,Y0,X1,Y1 shift=S caught=N focus=ID order=ID,... selected=ID,...
#   sim click (X, Y): ...                            a release below the threshold, or a right click
#   cursor: NAME at X Y (frame F)                    the world hover cursor's changes
#   clock: tick T paused bits B, ...
#   char ID "NAME" race ... flags 0xFLAGS ... at X Y Z ...
# Every check prints ok/FAIL; the exit code is 1 when any failed. run.sh keeps this file's output as OUT/check.txt.

BEGIN {
	# the boxes box.txt asks for (the logged rect x0,y0,x1,y1: the press point and the release point)
	BOX_ABC = "330,300,700,560"		# a plain box over Alpha, Bravo and Charlie
	BOX_DELTA = "715,300,800,560"		# Shift + a box over Delta
	BOX_EMPTY = "100,200,230,400"		# empty ground
	BOX_46 = "150,250,154,256"		# the 4x6 drag: a click, never a box
	BOX_55 = "150,300,155,305"		# the 5x5 drag: a box
	BOX_DEAD = "1000,210,1085,300"		# over DeadOne (dead)
	BOX_KO = "140,430,270,630"		# over KOOne (KO'd)
	BOX_RMB = "720,260,950,560"		# a right press mid-drag
	BOX_PAUSED = "325,305,705,555"		# paused
	BOX_FOLLOW = "400,455,880,650"		# following (V direct drive)
	BOX_FAR = "500,540,700,250"		# over FarOne at 8000 u from the camera
	BOX_NEAR = "505,545,705,255"		# over FarOne at 7000 u
	known[BOX_ABC] = known[BOX_DELTA] = known[BOX_EMPTY] = known[BOX_55] = known[BOX_DEAD] = 1
	known[BOX_KO] = known[BOX_RMB] = known[BOX_PAUSED] = known[BOX_FOLLOW] = known[BOX_FAR] = known[BOX_NEAR] = 1
}

function ok(m) { printf("ok   %s\n", m) }
function bad(m) { printf("FAIL %s\n", m); fails++ }

# 0x hex -> decimal (no strtonum: this must run under any awk)
function hex(s,   i, n, v, d)
{
	v = 0
	n = length(s)
	for (i = 1; i <= n; i++)
	{
		d = index("0123456789abcdef", tolower(substr(s, i, 1))) - 1
		if (d < 0) { break }
		v = v * 16 + d
	}
	return v
}

function split_count(names,   a) { return names == "" ? 0 : split(names, a, ",") }

# The names of an id list, in the log's order, joined by commas ("none" for none).
function ids_names(list,   a, n, i, s)
{
	n = split(list, a, ",")
	if (n == 0 || (n == 1 && a[1] == "")) { return "none" }
	s = ""
	for (i = 1; i <= n; i++) { s = s (i > 1 ? "," : "") (a[i] in id2nm ? id2nm[a[i]] : a[i]) }
	return s
}

# The names' sim ids, sorted as the client selects them (id order), joined by commas.
function ids_of(names,   a, n, i, j, t, arr, s)
{
	n = split(names, a, ",")
	if (n == 0 || (n == 1 && a[1] == "")) { return "" }
	for (i = 1; i <= n; i++) { arr[i] = nm2id[a[i]] + 0 }
	for (i = 2; i <= n; i++) { t = arr[i]; for (j = i - 1; j >= 1 && arr[j] > t; j--) { arr[j + 1] = arr[j] } arr[j + 1] = t }
	s = ""
	for (i = 1; i <= n; i++) { s = s (i > 1 ? "," : "") sprintf("%.0f", arr[i]) }
	return s
}

# One release: the caught names (in selection order), the focus's name, the whole selection afterwards, the Shift state.
function expect_box(rect, caught, focus, selected, shift, note)
{
	if (!(rect in boxcaught)) { bad(note ": no box select line for " rect); return }
	ok(note)
	if (boxorder[rect] != ids_of(caught)) { bad(note ": caught " ids_names(boxorder[rect]) " (want " (caught == "" ? "none" : caught) ")") }
	if (boxcaught[rect] != split_count(caught)) { bad(note ": caught=" boxcaught[rect] " (want " split_count(caught) ")") }
	if (shift >= 0 && boxshift[rect] != shift) { bad(note ": shift=" boxshift[rect] " (want " shift ")") }
	if (focus != "")
	{
		if (boxfocus[rect] != nm2id[focus]) { bad(note ": focus " (boxfocus[rect] in id2nm ? id2nm[boxfocus[rect]] : boxfocus[rect]) " (want " focus ")") }
		else { ok(note ": focus " focus) }
	}
	if (selected != "" && boxselected[rect] != ids_of(selected)) { bad(note ": selection " ids_names(boxselected[rect]) " (want " selected ")") }
}

/^char [0-9]+ "/ {
	split($0, q, "\"")
	split(q[1], h, " ")
	nm2id[q[2]] = h[2]
	id2nm[h[2]] = q[2]
	f = $0
	sub(/.* flags 0x/, "", f)
	sub(/[^0-9a-fA-F].*/, "", f)
	flags[h[2]] = hex(f)
	next
}

/^box select / {
	delete kv
	for (i = 3; i <= NF; i++) { p = index($i, "="); if (p > 0) { kv[substr($i, 1, p - 1)] = substr($i, p + 1) } }
	r = kv["rect"]
	boxcaught[r] = kv["caught"]
	boxfocus[r] = kv["focus"]
	boxorder[r] = kv["order"]
	boxselected[r] = kv["selected"]
	boxshift[r] = kv["shift"]
	boxframe[r] = kv["frame"]
	seens[++seen] = r
	next
}

/^sim click / {
	s = $0
	sub(/^sim click \(/, "", s)
	sub(/\).*/, "", s)
	clicks[s]++
	next
}

/^cursor: / {
	f = $0
	sub(/.*\(frame /, "", f)
	sub(/\).*/, "", f)
	curframe[f] = 1
	next
}

/^clock: / {
	if ($0 ~ /paused bits [0-9]*[13579],/) { paused = 1 }
	next
}

END {
	split("Alpha Bravo Charlie Delta Echo KOOne DeadOne FarOne", names, " ")
	for (i = 1; i <= 8; i++) { if (!(names[i] in nm2id)) { bad("no `char` line for " names[i] " (dump chars)") } }

	# the fixture: DeadOne is dead on the wire (kDead, bit 16)
	if (!("DeadOne" in nm2id)) { }
	else if (flags[nm2id["DeadOne"]] % 32 < 16) { bad("DeadOne is not dead on the wire (flags 0x" sprintf("%x", flags[nm2id["DeadOne"]]) ")") }
	else { ok("DeadOne is dead on the wire (flags 0x" sprintf("%x", flags[nm2id["DeadOne"]]) ")") }

	# 1, 4: a plain box selects the three and the focus is the last caught
	expect_box(BOX_ABC, "Alpha,Bravo,Charlie", "Charlie", "Alpha,Bravo,Charlie", 0, "plain box over Alpha..Charlie")
	expect_box(BOX_DELTA, "Delta", "Delta", "Alpha,Bravo,Charlie,Delta", 1, "Shift box over Delta")
	expect_box(BOX_EMPTY, "", "Delta", "Alpha,Bravo,Charlie,Delta", 0, "box over empty ground")

	# 1: the threshold, both axes past 4 px
	if (BOX_46 in boxcaught) { bad("a 4x6 drag started a box") } else { ok("a 4x6 drag is a click") }
	if (!("150, 250" in clicks)) { bad("the 4x6 drag's release was not the ordinary click") } else { ok("the 4x6 drag's release clicked") }
	expect_box(BOX_55, "", "Delta", "Alpha,Bravo,Charlie,Delta", 0, "a 5x5 drag is a box")

	# 3: dead ones are left out, KO'd ones are boxed
	expect_box(BOX_DEAD, "", "Delta", "Alpha,Bravo,Charlie,Delta", 0, "box over DeadOne (dead)")
	expect_box(BOX_KO, "KOOne", "KOOne", "KOOne", 0, "box over KOOne (KO'd)")

	# 5: a right press mid-drag orders nothing, the box keeps going and the hover cursor holds
	expect_box(BOX_RMB, "Delta,Echo", "Echo", "Delta,Echo", 0, "box with a right press mid-drag")
	if ("800, 380" in clicks) { bad("the right press mid-drag ordered (sim click at 800, 380)") } else { ok("the right press mid-drag ordered nothing") }
	if (BOX_RMB in boxcaught)
	{
		st = boxframe[BOX_RMB] + 0	# the drag runs frames 604..614 in box.txt: the press is 602, the release 614
		ncur = 0
		for (f in curframe) { if (f + 0 >= st - 12 && f + 0 <= st) { ncur++ } }
		if (ncur) { bad("the hover cursor changed during the drag (" ncur " cursor lines)") } else { ok("the hover cursor held during the drag") }
	}

	# 7: paused
	if (!paused) { bad("the sim was not paused for the paused box (no `clock` line with bit 0)") } else { ok("the sim was paused") }
	expect_box(BOX_PAUSED, "Alpha,Bravo,Charlie", "Charlie", "Alpha,Bravo,Charlie", 0, "box while paused")

	# 7: following (V direct drive); the free camera is every other box (run.sh compares the V shots' camera lines)
	expect_box(BOX_FOLLOW, "Alpha,Bravo,Charlie,Delta,Echo", "Echo", "Alpha,Bravo,Charlie,Delta,Echo", 0, "box while following")

	# 2: the reach, 7500 u from the camera
	expect_box(BOX_FAR, "", "Echo", "Alpha,Bravo,Charlie,Delta,Echo", 0, "FarOne at 8000 u is out of reach")
	expect_box(BOX_NEAR, "FarOne", "FarOne", "FarOne", 0, "FarOne at 7000 u is in reach")

	# no box the script did not ask for
	for (i = 1; i <= seen; i++) { if (!(seens[i] in known)) { bad("an unexpected box at " seens[i] " (frame " boxframe[seens[i]] ")") } }
	if (seen == 0) { bad("no box select lines at all") }
	else { ok(seen " box releases") }
	if (fails) { printf("boxselect: %d FAIL\n", fails); exit 1 }
	printf("boxselect: all checks ok\n")
}
