#!/bin/bash
# beams_air (kenshi-nun5): run.sh OUT. FAILS when the lamp cone is not drawn in the air. The Hub bar at 01:00 from the floor looking up at the shaded lamp (the
# lampbeams scene, bar.txt), render.lamp_beams 0 (OUT/b0) against 1 (OUT/b1), in the overlap mode and, with LOCK=1 (default), in lockstep (b0_lock / b1_lock).
# The cone region is the air under the lamp, left of the lit wall (x 300..750, y 200..450 of the 1280x720 shot, no wall, no HUD): mean(b1 - b0) there must reach MIN
# (default 12). Measured: 7.2 on the broken build (the floor cut clipped every ray that starts above the cut plane: the lamp light only showed on the wall), 20.7 fixed.
# Needs a game-data tree like lampbeams (SKIP otherwise). Port: PORT (47834). Exit 1 on a failure.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$here/../need_data.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
min=${MIN:-12}
modes="b0 b1"; [ "${LOCK:-1}" = 1 ] && modes="$modes b0_lock b1_lock"
MODES="$modes" bash "$here/../lampbeams/run.sh" "$out" || exit 1
fail=0
for pair in "b0 b1" "b0_lock b1_lock"; do
	set -- $pair
	[ -f "$out/$1/night.png" ] && [ -f "$out/$2/night.png" ] || { [ "$1" = b0_lock ] && [ "${LOCK:-1}" != 1 ] && continue; echo "beams_air $2: FAIL (no night shot)"; fail=1; continue; }
	d=$(python3 -I - "$out/$1/night.png" "$out/$2/night.png" <<'P'
import sys
from PIL import Image
import numpy as np
a, b = (np.asarray(Image.open(p).convert('RGB')).astype(float) for p in sys.argv[1:3])
r = (slice(200, 450), slice(300, 750))
print('%.2f' % (b[r] - a[r]).mean())
P
)
	if python3 -I -c "import sys; sys.exit(0 if float('$d') >= float('$min') else 1)"; then echo "beams_air $2: PASS cone diff $d >= $min"; else echo "beams_air $2: FAIL cone diff $d < $min (the lamp cone is not drawn in the air)"; fail=1; fi
done
[ "${CONE:-1}" = 1 ] && { bash "$here/cone.sh" "$out/cone" || fail=1; }	# each lamp's cone from the bulb, from the ask's camera and one near the lamp (kenshi-ac8f)
exit $fail
