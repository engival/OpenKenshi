# Shared end-of-run check for client/tests/*/run.sh (kenshi-k16.2.308): a suite must exit non-zero when its run did nothing (a client that quit
# early, a broken script, so no shot was written), not only when the client's own exit status says so. Sourced, not run:
#   run_status RC SCRIPT [MIN_SHOTS]
#     true when RC is 0 and every `screenshot PATH` line the filled SCRIPT asks for is a non-empty file (and there are at least MIN_SHOTS of
#     them, default 0). Prints the counts; each missing shot goes to stderr. A script with no screenshot op needs MIN_SHOTS 0 and a check of
#     its own.
run_status() {
	local rc=$1 script=$2 min=${3:-0} want=0 have=0 f
	while IFS= read -r f
	do
		want=$((want + 1))
		if [ -s "$f" ]; then have=$((have + 1)); else echo "run_status: no shot $f" >&2; fi
	done < <(sed -n 's/^[0-9][0-9]* screenshot \(.*\)$/\1/p' "$script")
	echo "run_status: client rc $rc, shots $have of $want"
	[ "$rc" -eq 0 ] && [ "$want" -ge "$min" ] && [ "$have" -eq "$want" ]
}
